// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/ipc/service/shared_image_stub.h"

#include <inttypes.h>

#include "base/trace_event/memory_dump_manager.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_image_factory.h"
#include "gpu/ipc/common/command_buffer_id.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "gpu/ipc/service/gpu_channel_manager.h"
#include "gpu/ipc/service/gpu_memory_buffer_factory.h"
#include "ui/gl/gl_context.h"

namespace gpu {

SharedImageStub::SharedImageStub(GpuChannel* channel, int32_t route_id)
    : channel_(channel),
      sequence_(channel->scheduler()->CreateSequence(SchedulingPriority::kLow)),
      sync_point_client_state_(
          channel->sync_point_manager()->CreateSyncPointClientState(
              CommandBufferNamespace::GPU_IO,
              CommandBufferIdFromChannelAndRoute(channel->client_id(),
                                                 route_id),
              sequence_)) {
  base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(
      this, "gpu::SharedImageStub", channel_->task_runner());
}

SharedImageStub::~SharedImageStub() {
  channel_->scheduler()->DestroySequence(sequence_);
  sync_point_client_state_->Destroy();
  if (factory_ && factory_->HasImages()) {
    bool have_context = MakeContextCurrent();
    factory_->DestroyAllSharedImages(have_context);
  }
  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      this);
}

std::unique_ptr<SharedImageStub> SharedImageStub::Create(GpuChannel* channel,
                                                         int32_t route_id) {
  auto stub = base::WrapUnique(new SharedImageStub(channel, route_id));
  ContextResult result = stub->MakeContextCurrentAndCreateFactory();
  if (result == ContextResult::kSuccess)
    return stub;

  // If it's not a transient failure, treat it as fatal.
  if (result != ContextResult::kTransientFailure)
    return nullptr;

  // For transient failure, retry once to create a shared context state and
  // hence factory again.
  if (stub->MakeContextCurrentAndCreateFactory() != ContextResult::kSuccess)
    return nullptr;
  return stub;
}

bool SharedImageStub::OnMessageReceived(const IPC::Message& msg) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(SharedImageStub, msg)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_CreateSharedImage, OnCreateSharedImage)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_CreateSharedImageWithData,
                        OnCreateSharedImageWithData)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_CreateGMBSharedImage,
                        OnCreateGMBSharedImage)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_UpdateSharedImage, OnUpdateSharedImage)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_DestroySharedImage, OnDestroySharedImage)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_RegisterSharedImageUploadBuffer,
                        OnRegisterSharedImageUploadBuffer)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void SharedImageStub::OnCreateSharedImage(
    const GpuChannelMsg_CreateSharedImage_Params& params) {
  TRACE_EVENT2("gpu", "SharedImageStub::OnCreateSharedImage", "width",
               params.size.width(), "height", params.size.height());
  if (!params.mailbox.IsSharedImage()) {
    LOG(ERROR) << "SharedImageStub: Trying to create a SharedImage with a "
                  "non-SharedImage mailbox.";
    OnError();
    return;
  }

  if (!MakeContextCurrent()) {
    OnError();
    return;
  }

  if (!factory_->CreateSharedImage(params.mailbox, params.format, params.size,
                                   params.color_space, params.usage)) {
    LOG(ERROR) << "SharedImageStub: Unable to create shared image";
    OnError();
    return;
  }

  SyncToken sync_token(sync_point_client_state_->namespace_id(),
                       sync_point_client_state_->command_buffer_id(),
                       params.release_id);
  auto* mailbox_manager = channel_->gpu_channel_manager()->mailbox_manager();
  mailbox_manager->PushTextureUpdates(sync_token);
  sync_point_client_state_->ReleaseFenceSync(params.release_id);
}

void SharedImageStub::OnCreateSharedImageWithData(
    const GpuChannelMsg_CreateSharedImageWithData_Params& params) {
  TRACE_EVENT2("gpu", "SharedImageStub::OnCreateSharedImageWithData", "width",
               params.size.width(), "height", params.size.height());
  if (!params.mailbox.IsSharedImage()) {
    LOG(ERROR) << "SharedImageStub: Trying to create a SharedImage with a "
                  "non-SharedImage mailbox.";
    OnError();
    return;
  }

  if (!MakeContextCurrent()) {
    OnError();
    return;
  }

  base::CheckedNumeric<size_t> safe_required_span_size =
      params.pixel_data_offset;
  safe_required_span_size += params.pixel_data_size;
  size_t required_span_size;
  if (!safe_required_span_size.AssignIfValid(&required_span_size)) {
    LOG(ERROR) << "SharedImageStub: upload data size and offset is invalid";
    OnError();
    return;
  }

  auto memory =
      upload_memory_mapping_.GetMemoryAsSpan<uint8_t>(required_span_size);
  if (memory.empty()) {
    LOG(ERROR) << "SharedImageStub: upload data does not have expected size";
    OnError();
    return;
  }

  auto subspan =
      memory.subspan(params.pixel_data_offset, params.pixel_data_size);

  if (!factory_->CreateSharedImage(params.mailbox, params.format, params.size,
                                   params.color_space, params.usage, subspan)) {
    LOG(ERROR) << "SharedImageStub: Unable to create shared image";
    OnError();
    return;
  }

  // If this is the last upload using a given buffer, release it.
  if (params.done_with_shm) {
    upload_memory_mapping_ = base::ReadOnlySharedMemoryMapping();
    upload_memory_ = base::ReadOnlySharedMemoryRegion();
  }

  SyncToken sync_token(sync_point_client_state_->namespace_id(),
                       sync_point_client_state_->command_buffer_id(),
                       params.release_id);
  auto* mailbox_manager = channel_->gpu_channel_manager()->mailbox_manager();
  mailbox_manager->PushTextureUpdates(sync_token);
  sync_point_client_state_->ReleaseFenceSync(params.release_id);
}

void SharedImageStub::OnCreateGMBSharedImage(
    GpuChannelMsg_CreateGMBSharedImage_Params params) {
  TRACE_EVENT2("gpu", "SharedImageStub::OnCreateGMBSharedImage", "width",
               params.size.width(), "height", params.size.height());
  if (!params.mailbox.IsSharedImage()) {
    LOG(ERROR) << "SharedImageStub: Trying to create a SharedImage with a "
                  "non-SharedImage mailbox.";
    OnError();
    return;
  }

  if (!MakeContextCurrent()) {
    OnError();
    return;
  }

  // TODO(piman): add support for SurfaceHandle (for backbuffers for ozone/drm).
  SurfaceHandle surface_handle = kNullSurfaceHandle;
  if (!factory_->CreateSharedImage(params.mailbox, channel_->client_id(),
                                   std::move(params.handle), params.format,
                                   surface_handle, params.size,
                                   params.color_space, params.usage)) {
    LOG(ERROR) << "SharedImageStub: Unable to create shared image";
    OnError();
    return;
  }

  SyncToken sync_token(sync_point_client_state_->namespace_id(),
                       sync_point_client_state_->command_buffer_id(),
                       params.release_id);
  auto* mailbox_manager = channel_->gpu_channel_manager()->mailbox_manager();
  mailbox_manager->PushTextureUpdates(sync_token);
  sync_point_client_state_->ReleaseFenceSync(params.release_id);
}

void SharedImageStub::OnUpdateSharedImage(const Mailbox& mailbox,
                                          uint32_t release_id) {
  TRACE_EVENT0("gpu", "SharedImageStub::OnUpdateSharedImage");
  if (!mailbox.IsSharedImage()) {
    LOG(ERROR) << "SharedImageStub: Trying to access a SharedImage with a "
                  "non-SharedImage mailbox.";
    OnError();
    return;
  }

  if (!MakeContextCurrent()) {
    OnError();
    return;
  }

  if (!factory_->UpdateSharedImage(mailbox)) {
    LOG(ERROR) << "SharedImageStub: Unable to destroy shared image";
    OnError();
    return;
  }

  SyncToken sync_token(sync_point_client_state_->namespace_id(),
                       sync_point_client_state_->command_buffer_id(),
                       release_id);
  auto* mailbox_manager = channel_->gpu_channel_manager()->mailbox_manager();
  mailbox_manager->PushTextureUpdates(sync_token);
  sync_point_client_state_->ReleaseFenceSync(release_id);
}

void SharedImageStub::OnDestroySharedImage(const Mailbox& mailbox) {
  TRACE_EVENT0("gpu", "SharedImageStub::OnDestroySharedImage");
  if (!mailbox.IsSharedImage()) {
    LOG(ERROR) << "SharedImageStub: Trying to destroy a SharedImage with a "
                  "non-SharedImage mailbox.";
    OnError();
    return;
  }

  if (!MakeContextCurrent()) {
    OnError();
    return;
  }

  if (!factory_->DestroySharedImage(mailbox)) {
    LOG(ERROR) << "SharedImageStub: Unable to destroy shared image";
    OnError();
    return;
  }
}

void SharedImageStub::OnRegisterSharedImageUploadBuffer(
    base::ReadOnlySharedMemoryRegion shm) {
  TRACE_EVENT0("gpu", "SharedImageStub::OnRegisterSharedImageUploadBuffer");
  upload_memory_ = std::move(shm);
  upload_memory_mapping_ = upload_memory_.Map();
  if (!upload_memory_mapping_.IsValid()) {
    LOG(ERROR)
        << "SharedImageStub: Unable to map shared memory for upload data";
    OnError();
    return;
  }
}

bool SharedImageStub::MakeContextCurrent() {
  DCHECK(context_state_);

  if (context_state_->context_lost()) {
    LOG(ERROR) << "SharedImageStub: context already lost";
    return false;
  }

  // |factory_| never writes to the surface, so pass nullptr to
  // improve performance. https://crbug.com/457431
  auto* context = context_state_->real_context();
  if (context->IsCurrent(nullptr) ||
      context_state_->real_context()->MakeCurrent(context_state_->surface())) {
    CHECK(gl::GLContext::GetCurrent());
    return true;
  } else {
    context_state_->MarkContextLost();
    LOG(ERROR) << "SharedImageStub: MakeCurrent failed";
    return false;
  }
}

ContextResult SharedImageStub::MakeContextCurrentAndCreateFactory() {
  auto* channel_manager = channel_->gpu_channel_manager();
  DCHECK(!context_state_);
  ContextResult result;
  context_state_ = channel_manager->GetSharedContextState(&result);
  if (result != ContextResult::kSuccess) {
    LOG(ERROR) << "SharedImageStub: unable to create context";
    context_state_ = nullptr;
    return result;
  }
  DCHECK(context_state_);
  DCHECK(!context_state_->context_lost());
  if (!MakeContextCurrent()) {
    context_state_ = nullptr;
    return ContextResult::kTransientFailure;
  }

  gpu::GpuMemoryBufferFactory* gmb_factory =
      channel_manager->gpu_memory_buffer_factory();
  factory_ = std::make_unique<SharedImageFactory>(
      channel_manager->gpu_preferences(),
      channel_manager->gpu_driver_bug_workarounds(),
      channel_manager->gpu_feature_info(), context_state_.get(),
      channel_manager->mailbox_manager(),
      channel_manager->shared_image_manager(),
      gmb_factory ? gmb_factory->AsImageFactory() : nullptr, this);
  return ContextResult::kSuccess;
}

void SharedImageStub::OnError() {
  channel_->OnChannelError();
}

void SharedImageStub::TrackMemoryAllocatedChange(uint64_t delta) {
  size_ += delta;
}

uint64_t SharedImageStub::GetSize() const {
  return size_;
}

uint64_t SharedImageStub::ClientTracingId() const {
  return channel_->client_tracing_id();
}

int SharedImageStub::ClientId() const {
  return channel_->client_id();
}

uint64_t SharedImageStub::ContextGroupTracingId() const {
  return sync_point_client_state_->command_buffer_id().GetUnsafeValue();
}

bool SharedImageStub::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd) {
  if (!factory_)
    return true;

  if (args.level_of_detail ==
      base::trace_event::MemoryDumpLevelOfDetail::BACKGROUND) {
    std::string dump_name =
        base::StringPrintf("gpu/gl/textures/client_0x%" PRIX32, ClientId());
    base::trace_event::MemoryAllocatorDump* dump =
        pmd->CreateAllocatorDump(dump_name);
    dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                    base::trace_event::MemoryAllocatorDump::kUnitsBytes, size_);

    // Early out, no need for more detail in a BACKGROUND dump.
    return true;
  }

  return factory_->OnMemoryDump(args, pmd, ClientId(), ClientTracingId());
}

}  // namespace gpu

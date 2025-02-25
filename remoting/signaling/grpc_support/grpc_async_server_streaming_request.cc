// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/signaling/grpc_support/grpc_async_server_streaming_request.h"

#include "base/bind.h"

namespace remoting {

GrpcAsyncServerStreamingRequestBase::GrpcAsyncServerStreamingRequestBase(
    std::unique_ptr<grpc::ClientContext> context,
    base::OnceCallback<void(const grpc::Status&)> on_channel_closed,
    std::unique_ptr<ScopedGrpcServerStream>* scoped_stream)
    : GrpcAsyncRequest(std::move(context)), weak_factory_(this) {
  DCHECK(on_channel_closed);
  DCHECK_NE(nullptr, scoped_stream);
  on_channel_closed_ = std::move(on_channel_closed);
  *scoped_stream =
      std::make_unique<ScopedGrpcServerStream>(weak_factory_.GetWeakPtr());
}

GrpcAsyncServerStreamingRequestBase::~GrpcAsyncServerStreamingRequestBase() =
    default;

bool GrpcAsyncServerStreamingRequestBase::OnDequeue(bool operation_succeeded) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::CLOSED) {
    return false;
  }
  if (state_ == State::FINISHING) {
    DCHECK(operation_succeeded);
    state_ = State::CLOSED;
    ResolveChannelClosed();
    return false;
  }
  if (!operation_succeeded) {
    VLOG(0) << "Can't read any more data. Figuring out the reason..."
            << " Streaming call: " << this;
    state_ = State::FINISHING;
    return true;
  }
  if (state_ == State::STARTING) {
    VLOG(0) << "Streaming call started: " << this;
    state_ = State::STREAMING;
    return true;
  }
  DCHECK_EQ(State::STREAMING, state_);
  VLOG(0) << "Streaming call received message: " << this;
  ResolveIncomingMessage();
  return true;
}

void GrpcAsyncServerStreamingRequestBase::Reenqueue(void* event_tag) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  switch (state_) {
    case State::STREAMING:
      WaitForIncomingMessage(event_tag);
      break;
    case State::FINISHING:
      FinishStream(event_tag);
      break;
    default:
      NOTREACHED() << "Unexpected state: " << static_cast<int>(state_);
      break;
  }
}

void GrpcAsyncServerStreamingRequestBase::OnRequestCanceled() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::CLOSED) {
    return;
  }
  state_ = State::CLOSED;
  status_ = grpc::Status::CANCELLED;
  weak_factory_.InvalidateWeakPtrs();
}

bool GrpcAsyncServerStreamingRequestBase::CanStartRequest() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::STARTING;
}

void GrpcAsyncServerStreamingRequestBase::ResolveChannelClosed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(run_task_callback_);
  run_task_callback_.Run(
      base::BindOnce(std::move(on_channel_closed_), status_));
}

}  // namespace remoting

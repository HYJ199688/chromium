// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/profiler/thread_delegate_win.h"

#include <windows.h>
#include <winternl.h>

#include "base/profiler/win32_stack_frame_unwinder.h"
#include "build/build_config.h"

// IMPORTANT NOTE: Some functions within this implementation are invoked while
// the target thread is suspended so it must not do any allocation from the
// heap, including indirectly via use of DCHECK/CHECK or other logging
// statements. Otherwise this code can deadlock on heap locks acquired by the
// target thread before it was suspended. These functions are commented with "NO
// HEAP ALLOCATIONS".

namespace base {

namespace {

// The thread environment block internal type.
struct TEB {
  NT_TIB Tib;
  // Rest of struct is ignored.
};

// Returns the thread environment block pointer for |thread_handle|.
const TEB* GetThreadEnvironmentBlock(HANDLE thread_handle) {
  // Define the internal types we need to invoke NtQueryInformationThread.
  enum THREAD_INFORMATION_CLASS { ThreadBasicInformation };

  struct CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
  };

  struct THREAD_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    TEB* Teb;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    LONG Priority;
    LONG BasePriority;
  };

  using NtQueryInformationThreadFunction =
      NTSTATUS(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, PVOID, ULONG, PULONG);

  const auto nt_query_information_thread =
      reinterpret_cast<NtQueryInformationThreadFunction>(::GetProcAddress(
          ::GetModuleHandle(L"ntdll.dll"), "NtQueryInformationThread"));
  if (!nt_query_information_thread)
    return nullptr;

  THREAD_BASIC_INFORMATION basic_info = {0};
  NTSTATUS status = nt_query_information_thread(
      thread_handle, ThreadBasicInformation, &basic_info,
      sizeof(THREAD_BASIC_INFORMATION), nullptr);
  if (status != 0)
    return nullptr;

  return basic_info.Teb;
}

// Tests whether |stack_pointer| points to a location in the guard page. NO HEAP
// ALLOCATIONS.
bool PointsToGuardPage(uintptr_t stack_pointer) {
  MEMORY_BASIC_INFORMATION memory_info;
  SIZE_T result = ::VirtualQuery(reinterpret_cast<LPCVOID>(stack_pointer),
                                 &memory_info, sizeof(memory_info));
  return result != 0 && (memory_info.Protect & PAGE_GUARD);
}

// ScopedDisablePriorityBoost -------------------------------------------------

// Disables priority boost on a thread for the lifetime of the object.
class ScopedDisablePriorityBoost {
 public:
  ScopedDisablePriorityBoost(HANDLE thread_handle);
  ~ScopedDisablePriorityBoost();

 private:
  HANDLE thread_handle_;
  BOOL got_previous_boost_state_;
  BOOL boost_state_was_disabled_;

  DISALLOW_COPY_AND_ASSIGN(ScopedDisablePriorityBoost);
};

// NO HEAP ALLOCATIONS.
ScopedDisablePriorityBoost::ScopedDisablePriorityBoost(HANDLE thread_handle)
    : thread_handle_(thread_handle),
      got_previous_boost_state_(false),
      boost_state_was_disabled_(false) {
  got_previous_boost_state_ =
      ::GetThreadPriorityBoost(thread_handle_, &boost_state_was_disabled_);
  if (got_previous_boost_state_) {
    // Confusingly, TRUE disables priority boost.
    ::SetThreadPriorityBoost(thread_handle_, TRUE);
  }
}

ScopedDisablePriorityBoost::~ScopedDisablePriorityBoost() {
  if (got_previous_boost_state_)
    ::SetThreadPriorityBoost(thread_handle_, boost_state_was_disabled_);
}

}  // namespace

// ScopedSuspendThread --------------------------------------------------------

// NO HEAP ALLOCATIONS after ::SuspendThread.
ThreadDelegateWin::ScopedSuspendThread::ScopedSuspendThread(
    HANDLE thread_handle)
    : thread_handle_(thread_handle),
      was_successful_(::SuspendThread(thread_handle) !=
                      static_cast<DWORD>(-1)) {}

// NO HEAP ALLOCATIONS. The CHECK is OK because it provides a more noisy failure
// mode than deadlocking.
ThreadDelegateWin::ScopedSuspendThread::~ScopedSuspendThread() {
  if (!was_successful_)
    return;

  // Disable the priority boost that the thread would otherwise receive on
  // resume. We do this to avoid artificially altering the dynamics of the
  // executing application any more than we already are by suspending and
  // resuming the thread.
  //
  // Note that this can racily disable a priority boost that otherwise would
  // have been given to the thread, if the thread is waiting on other wait
  // conditions at the time of SuspendThread and those conditions are satisfied
  // before priority boost is reenabled. The measured length of this window is
  // ~100us, so this should occur fairly rarely.
  ScopedDisablePriorityBoost disable_priority_boost(thread_handle_);
  bool resume_thread_succeeded =
      ::ResumeThread(thread_handle_) != static_cast<DWORD>(-1);
  CHECK(resume_thread_succeeded) << "ResumeThread failed: " << GetLastError();
}

bool ThreadDelegateWin::ScopedSuspendThread::WasSuccessful() const {
  return was_successful_;
}

// ThreadDelegateWin ----------------------------------------------------------

ThreadDelegateWin::ThreadDelegateWin(PlatformThreadId thread_id)
    : thread_handle_(::OpenThread(
          THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
          FALSE,
          thread_id)),
      thread_stack_base_address_(reinterpret_cast<uintptr_t>(
          GetThreadEnvironmentBlock(thread_handle_.Get())->Tib.StackBase)) {}

ThreadDelegateWin::~ThreadDelegateWin() = default;

std::unique_ptr<ThreadDelegate::ScopedSuspendThread>
ThreadDelegateWin::CreateScopedSuspendThread() {
  return std::make_unique<ScopedSuspendThread>(thread_handle_.Get());
}

// NO HEAP ALLOCATIONS.
bool ThreadDelegateWin::GetThreadContext(CONTEXT* thread_context) {
  *thread_context = {0};
  thread_context->ContextFlags = CONTEXT_FULL;
  return ::GetThreadContext(thread_handle_.Get(), thread_context) != 0;
}

// NO HEAP ALLOCATIONS.
uintptr_t ThreadDelegateWin::GetStackBaseAddress() const {
  return thread_stack_base_address_;
}

// Tests whether |stack_pointer| points to a location in the guard page. NO HEAP
// ALLOCATIONS.
bool ThreadDelegateWin::CanCopyStack(uintptr_t stack_pointer) {
  // Dereferencing a pointer in the guard page in a thread that doesn't own the
  // stack results in a STATUS_GUARD_PAGE_VIOLATION exception and a crash. This
  // occurs very rarely, but reliably over the population.
  return !PointsToGuardPage(stack_pointer);
}

std::vector<uintptr_t*> ThreadDelegateWin::GetRegistersToRewrite(
    CONTEXT* thread_context) {
  // Return the set of non-volatile registers.
  return {
#if defined(ARCH_CPU_X86_64)
    &thread_context->R12, &thread_context->R13, &thread_context->R14,
        &thread_context->R15, &thread_context->Rdi, &thread_context->Rsi,
        &thread_context->Rbx, &thread_context->Rbp, &thread_context->Rsp
#elif defined(ARCH_CPU_ARM64)
    &thread_context->X19, &thread_context->X20, &thread_context->X21,
        &thread_context->X22, &thread_context->X23, &thread_context->X24,
        &thread_context->X25, &thread_context->X26, &thread_context->X27,
        &thread_context->X28, &thread_context->Fp, &thread_context->Lr
#endif
  };
}

UnwindResult ThreadDelegateWin::WalkNativeFrames(
    CONTEXT* thread_context,
    uintptr_t stack_top,
    ModuleCache* module_cache,
    std::vector<ProfileBuilder::Frame>* stack) {
  // Record the first frame from the context values.
  const ModuleCache::Module* module =
      module_cache->GetModuleForAddress(ContextPC(thread_context));
  stack->emplace_back(ContextPC(thread_context), module);

  Win32StackFrameUnwinder frame_unwinder;
  for (;;) {
    if (!module) {
      // There's no loaded module containing the instruction pointer. This can
      // be due to executing code that is not in a module (e.g. V8 generated
      // code or runtime-generated code associated with third-party injected
      // DLLs). It can also be due to the the module having been unloaded since
      // we recorded the stack.  In the latter case the function unwind
      // information was part of the unloaded module, so it's not possible to
      // unwind further.
      //
      // If a module was found, it's still theoretically possible for the
      // detected module module to be different than the one that was loaded
      // when the stack was copied (i.e. if the module was unloaded and a
      // different module loaded in overlapping memory). This likely would cause
      // a crash, but has not been observed in practice.
      //
      // We return UNRECOGNIZED_FRAME on the optimistic assumption that this may
      // be a frame the AuxUnwinder knows how to handle (e.g. a frame in V8
      // generated code).
      return UnwindResult::UNRECOGNIZED_FRAME;
    }

    if (!frame_unwinder.TryUnwind(stack->size() == 1u, thread_context, module))
      return UnwindResult::ABORTED;

    if (ContextPC(thread_context) == 0)
      return UnwindResult::COMPLETED;

    // Record the frame to which we just unwound.
    module = module_cache->GetModuleForAddress(ContextPC(thread_context));
    stack->emplace_back(ContextPC(thread_context), module);
  }

  NOTREACHED();
  return UnwindResult::COMPLETED;
}

}  // namespace base

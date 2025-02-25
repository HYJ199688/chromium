/*
 * Copyright (C) 2008 Apple Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DOM_TIMER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DOM_TIMER_H_

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/dom/user_gesture_indicator.h"
#include "third_party/blink/renderer/core/execution_context/context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/bindings/name_client.h"
#include "third_party/blink/renderer/platform/bindings/trace_wrapper_member.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/timer.h"

namespace blink {

class ExecutionContext;
class ScheduledAction;

class CORE_EXPORT DOMTimer final : public GarbageCollectedFinalized<DOMTimer>,
                                   public ContextLifecycleObserver,
                                   public TimerBase,
                                   public NameClient {
  USING_GARBAGE_COLLECTED_MIXIN(DOMTimer);

 public:
  // Creates a new timer owned by the ExecutionContext, starts it and returns
  // its ID.
  static int Install(ExecutionContext*,
                     ScheduledAction*,
                     TimeDelta timeout,
                     bool single_shot);
  static void RemoveByID(ExecutionContext*, int timeout_id);

  DOMTimer(ExecutionContext*,
           ScheduledAction*,
           TimeDelta interval,
           bool single_shot,
           int timeout_id);
  ~DOMTimer() override;

  // ContextLifecycleObserver
  void ContextDestroyed(ExecutionContext*) override;

  // Eager finalization is needed to promptly stop this Timer object.
  // Otherwise timer events might fire at an object that's slated for
  // destruction (when lazily swept), but some of its members (m_action) may
  // already have been finalized & must not be accessed.
  EAGERLY_FINALIZE();
  void Trace(blink::Visitor*) override;
  const char* NameInHeapSnapshot() const override { return "DOMTimer"; }

  void Stop() override;

 private:
  friend class DOMTimerCoordinator;  // For Create().

  void Fired() override;

  scoped_refptr<base::SingleThreadTaskRunner> TimerTaskRunner() const override;

  int timeout_id_;
  int nesting_level_;
  TraceWrapperMember<ScheduledAction> action_;
  scoped_refptr<UserGestureToken> user_gesture_token_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DOM_TIMER_H_

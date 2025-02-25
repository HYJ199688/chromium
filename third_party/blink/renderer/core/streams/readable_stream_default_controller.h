// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_STREAMS_READABLE_STREAM_DEFAULT_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_STREAMS_READABLE_STREAM_DEFAULT_CONTROLLER_H_

#include "base/optional.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/bindings/trace_wrapper_member.h"
#include "third_party/blink/renderer/platform/bindings/trace_wrapper_v8_reference.h"
#include "v8/include/v8.h"

namespace blink {

class ExceptionState;
class QueueWithSizes;
class ReadableStreamNative;
class ScriptState;
class ScriptValue;
class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamPromiseResolver;
class StreamStartAlgorithm;

class ReadableStreamDefaultController : public ScriptWrappable {
  DEFINE_WRAPPERTYPEINFO();

 public:
  ReadableStreamDefaultController();

  // https://streams.spec.whatwg.org/#rs-default-controller-desired-size
  double desiredSize(bool& is_null) const;

  // https://streams.spec.whatwg.org/#rs-default-controller-close
  void close(ScriptState*, ExceptionState&);

  // https://streams.spec.whatwg.org/#rs-default-controller-enqueue
  void enqueue(ScriptState*, ExceptionState&);
  void enqueue(ScriptState*, ScriptValue chunk, ExceptionState&);

  // https://streams.spec.whatwg.org/#rs-default-controller-error
  void error(ScriptState*);
  void error(ScriptState*, ScriptValue e);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-close
  static void Close(ScriptState*, ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-enqueue
  static void Enqueue(ScriptState*,
                      ReadableStreamDefaultController*,
                      v8::Local<v8::Value> chunk,
                      ExceptionState&);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-error
  static void Error(ScriptState*,
                    ReadableStreamDefaultController*,
                    v8::Local<v8::Value> e);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-get-desired-size
  base::Optional<double> GetDesiredSize() const;

  void Trace(Visitor*) override;

 private:
  friend class ReadableStreamNative;
  friend class ReadableStreamDefaultReader;

  // https://streams.spec.whatwg.org/#rs-default-controller-private-cancel
  v8::Local<v8::Promise> CancelSteps(ScriptState*, v8::Local<v8::Value> reason);

  // https://streams.spec.whatwg.org/#rs-default-controller-private-pull
  StreamPromiseResolver* PullSteps(ScriptState*);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-call-pull-if-needed
  static void CallPullIfNeeded(ScriptState*, ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-should-call-pull
  static bool ShouldCallPull(const ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-clear-algorithms
  static void ClearAlgorithms(ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#rs-default-controller-has-backpressure
  static bool HasBackpressure(const ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#readable-stream-default-controller-can-close-or-enqueue
  static bool CanCloseOrEnqueue(const ReadableStreamDefaultController*);

  // https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller
  static void SetUp(ScriptState*,
                    ReadableStreamNative*,
                    ReadableStreamDefaultController*,
                    StreamStartAlgorithm* start_algorithm,
                    StreamAlgorithm* pull_algorithm,
                    StreamAlgorithm* cancel_algorithm,
                    double high_water_mark,
                    StrategySizeAlgorithm* size_algorithm,
                    bool enable_blink_lock_notifications,
                    ExceptionState&);

  // https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller-from-underlying-source
  static void SetUpFromUnderlyingSource(ScriptState*,
                                        ReadableStreamNative*,
                                        v8::Local<v8::Object> underlying_source,
                                        double high_water_mark,
                                        StrategySizeAlgorithm* size_algorithm,
                                        bool enable_blink_lock_notifications,
                                        ExceptionState&);

  static const char* EnqueueExceptionMessage(
      const ReadableStreamDefaultController*);

  // Boolean flags are grouped together to reduce object size. Verbs have been
  // added to the names in the standard to match Blink style.
  bool is_close_requested_ = false;
  bool will_pull_again_ = false;
  bool is_pulling_ = false;
  bool is_started_ = false;
  bool enable_blink_lock_notifications_ = false;
  TraceWrapperMember<StreamAlgorithm> cancel_algorithm_;
  TraceWrapperMember<ReadableStreamNative> controlled_readable_stream_;
  TraceWrapperMember<StreamAlgorithm> pull_algorithm_;
  TraceWrapperMember<QueueWithSizes> queue_;
  double strategy_high_water_mark_ = 0.0;
  TraceWrapperMember<StrategySizeAlgorithm> strategy_size_algorithm_;
  TraceWrapperV8Reference<v8::Object> lock_notify_target_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_STREAMS_READABLE_STREAM_DEFAULT_CONTROLLER_H_

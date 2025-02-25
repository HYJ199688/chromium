// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/streams/readable_stream.h"

#include "base/optional.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/bindings/core/v8/script_function.h"
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_testing.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_extras_test_utils.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_iterator_result_value.h"
#include "third_party/blink/renderer/core/messaging/message_channel.h"
#include "third_party/blink/renderer/core/streams/readable_stream_default_controller_interface.h"
#include "third_party/blink/renderer/core/streams/test_underlying_source.h"
#include "third_party/blink/renderer/core/streams/underlying_source_base.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/string_resource.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "v8/include/v8.h"

namespace blink {

namespace {

// Web platform tests test ReadableStream more thoroughly from scripts.
class ReadableStreamTest : public testing::TestWithParam<bool> {
 public:
  ReadableStreamTest() : feature_(GetParam()) {}

  base::Optional<String> ReadAll(V8TestingScope& scope,
                                 ReadableStream* stream) {
    ScriptState* script_state = scope.GetScriptState();
    v8::Isolate* isolate = script_state->GetIsolate();
    v8::Local<v8::Context> context = script_state->GetContext();
    v8::Local<v8::Value> v8_stream = ToV8(stream, context->Global(), isolate);
    v8::Local<v8::Object> global = context->Global();

    bool set_result = false;
    if (!global->Set(context, V8String(isolate, "stream"), v8_stream)
             .To(&set_result)) {
      ADD_FAILURE();
      return base::nullopt;
    }

    const char script[] =
        R"JS(;
result = undefined;
async function readAll(stream) {
  const reader = stream.getReader();
  let temp = "";
  while (true) {
    const v = await reader.read();
    if (v.done) {
      result = temp;
      return;
    }
    temp = temp + v.value;
  }
}
readAll(stream);
)JS";

    if (EvalWithPrintingError(&scope, script).IsEmpty()) {
      ADD_FAILURE();
      return base::nullopt;
    }

    while (true) {
      v8::Local<v8::Value> result;
      if (!global->Get(context, V8String(isolate, "result")).ToLocal(&result)) {
        ADD_FAILURE();
        return base::nullopt;
      }
      if (!result->IsUndefined()) {
        DCHECK(result->IsString());
        return ToCoreString(result.As<v8::String>());
      }

      // Need to run the event loop for the Serialize test to pass messages
      // through the MessagePort.
      test::RunPendingTasks();

      // Allow Promises to resolve.
      v8::MicrotasksScope::PerformCheckpoint(isolate);
    }
    NOTREACHED();
    return base::nullopt;
  }

 private:
  ScopedStreamsNativeForTest feature_;
};

TEST_P(ReadableStreamTest, CreateWithoutArguments) {
  V8TestingScope scope;

  ReadableStream* stream =
      ReadableStream::Create(scope.GetScriptState(), scope.GetExceptionState());
  ASSERT_TRUE(stream);
  ASSERT_FALSE(scope.GetExceptionState().HadException());
}

TEST_P(ReadableStreamTest, CreateWithUnderlyingSourceOnly) {
  V8TestingScope scope;
  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(scope.GetScriptState());
  ScriptValue js_underlying_source = ScriptValue(
      scope.GetScriptState(),
      ToV8(underlying_source, scope.GetScriptState()->GetContext()->Global(),
           scope.GetScriptState()->GetIsolate()));

  EXPECT_FALSE(underlying_source->IsStartCalled());

  ReadableStream* stream = ReadableStream::Create(
      scope.GetScriptState(), js_underlying_source, scope.GetExceptionState());

  ASSERT_TRUE(stream);
  ASSERT_FALSE(scope.GetExceptionState().HadException());
  EXPECT_TRUE(underlying_source->IsStartCalled());
}

TEST_P(ReadableStreamTest, CreateWithFullArguments) {
  V8TestingScope scope;
  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(scope.GetScriptState());
  ScriptValue js_underlying_source = ScriptValue(
      scope.GetScriptState(),
      ToV8(underlying_source, scope.GetScriptState()->GetContext()->Global(),
           scope.GetScriptState()->GetIsolate()));
  ScriptValue js_empty_strategy = EvalWithPrintingError(&scope, "{}");
  ASSERT_FALSE(js_empty_strategy.IsEmpty());
  ReadableStream* stream =
      ReadableStream::Create(scope.GetScriptState(), js_underlying_source,
                             js_empty_strategy, scope.GetExceptionState());
  ASSERT_TRUE(stream);
  ASSERT_FALSE(scope.GetExceptionState().HadException());
  EXPECT_TRUE(underlying_source->IsStartCalled());
}

TEST_P(ReadableStreamTest, CreateWithPathologicalStrategy) {
  V8TestingScope scope;
  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(scope.GetScriptState());
  ScriptValue js_underlying_source = ScriptValue(
      scope.GetScriptState(),
      ToV8(underlying_source, scope.GetScriptState()->GetContext()->Global(),
           scope.GetScriptState()->GetIsolate()));
  ScriptValue js_pathological_strategy =
      EvalWithPrintingError(&scope, "({get size() { throw Error('e'); }})");
  ASSERT_FALSE(js_pathological_strategy.IsEmpty());

  ReadableStream* stream = ReadableStream::Create(
      scope.GetScriptState(), js_underlying_source, js_pathological_strategy,
      scope.GetExceptionState());
  ASSERT_FALSE(stream);
  ASSERT_TRUE(scope.GetExceptionState().HadException());
  EXPECT_FALSE(underlying_source->IsStartCalled());
}

// Testing getReader, locked, IsLocked and IsDisturbed.
TEST_P(ReadableStreamTest, GetReader) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  ScriptValue js_underlying_source =
      ScriptValue(script_state,
                  ToV8(underlying_source, script_state->GetContext()->Global(),
                       script_state->GetIsolate()));
  ReadableStream* stream = ReadableStream::Create(
      script_state, js_underlying_source, ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(stream);

  EXPECT_EQ(stream->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));
  EXPECT_EQ(stream->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));
  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));

  ScriptValue reader = stream->getReader(script_state, ASSERT_NO_EXCEPTION);

  EXPECT_TRUE(stream->locked(script_state, ASSERT_NO_EXCEPTION));
  EXPECT_EQ(stream->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(true));
  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));

  ASSERT_TRUE(reader.V8Value()->IsObject());
  v8::Local<v8::Object> v8_reader = reader.V8Value().As<v8::Object>();

  v8::Local<v8::Value> v8_read;
  ASSERT_TRUE(v8_reader
                  ->Get(script_state->GetContext(),
                        V8String(script_state->GetIsolate(), "read"))
                  .ToLocal(&v8_read));
  ASSERT_TRUE(v8_read->IsFunction());

  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));

  v8::Local<v8::Value> v8_read_result;
  v8::Local<v8::Value> args[] = {};
  ASSERT_TRUE(v8_read.As<v8::Function>()
                  ->Call(script_state->GetContext(), v8_reader, 0, args)
                  .ToLocal(&v8_read_result));

  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(true));
}

TEST_P(ReadableStreamTest, Cancel) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  ScriptValue js_underlying_source =
      ScriptValue(script_state,
                  ToV8(underlying_source, script_state->GetContext()->Global(),
                       script_state->GetIsolate()));
  ReadableStream* stream = ReadableStream::Create(
      script_state, js_underlying_source, ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(stream);

  EXPECT_FALSE(underlying_source->IsCancelled());
  EXPECT_FALSE(underlying_source->IsCancelledWithUndefined());
  EXPECT_FALSE(underlying_source->IsCancelledWithNull());

  stream->cancel(script_state, ASSERT_NO_EXCEPTION);

  EXPECT_TRUE(underlying_source->IsCancelled());
  EXPECT_TRUE(underlying_source->IsCancelledWithUndefined());
  EXPECT_FALSE(underlying_source->IsCancelledWithNull());
}

TEST_P(ReadableStreamTest, CancelWithNull) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  ScriptValue js_underlying_source =
      ScriptValue(script_state,
                  ToV8(underlying_source, script_state->GetContext()->Global(),
                       script_state->GetIsolate()));
  ReadableStream* stream = ReadableStream::Create(
      script_state, js_underlying_source, ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(stream);

  EXPECT_FALSE(underlying_source->IsCancelled());
  EXPECT_FALSE(underlying_source->IsCancelledWithUndefined());
  EXPECT_FALSE(underlying_source->IsCancelledWithNull());

  stream->cancel(
      script_state,
      ScriptValue(script_state, v8::Null(script_state->GetIsolate())),
      ASSERT_NO_EXCEPTION);

  EXPECT_TRUE(underlying_source->IsCancelled());
  EXPECT_FALSE(underlying_source->IsCancelledWithUndefined());
  EXPECT_TRUE(underlying_source->IsCancelledWithNull());
}

// TODO(yhirano): Write tests for pipeThrough and pipeTo.

TEST_P(ReadableStreamTest, Tee) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  ScriptValue js_underlying_source =
      ScriptValue(script_state,
                  ToV8(underlying_source, script_state->GetContext()->Global(),
                       script_state->GetIsolate()));
  ReadableStream* stream = ReadableStream::Create(
      script_state, js_underlying_source, ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(stream);

  underlying_source->Enqueue(
      ScriptValue(script_state, V8String(script_state->GetIsolate(), "hello")));
  underlying_source->Enqueue(
      ScriptValue(script_state, V8String(script_state->GetIsolate(), ", bye")));
  underlying_source->Close();

  ReadableStream* branch1 = nullptr;
  ReadableStream* branch2 = nullptr;
  stream->Tee(script_state, &branch1, &branch2, ASSERT_NO_EXCEPTION);

  EXPECT_EQ(stream->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(true));
  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));

  ASSERT_TRUE(branch1);
  ASSERT_TRUE(branch2);

  EXPECT_EQ(branch1->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));
  EXPECT_EQ(branch1->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));
  EXPECT_EQ(branch2->IsLocked(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));
  EXPECT_EQ(branch2->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(false));

  auto result1 = ReadAll(scope, branch1);
  ASSERT_TRUE(result1);
  EXPECT_EQ(*result1, "hello, bye");

  EXPECT_EQ(stream->IsDisturbed(script_state, ASSERT_NO_EXCEPTION),
            base::make_optional(true));

  auto result2 = ReadAll(scope, branch2);
  ASSERT_TRUE(result2);
  EXPECT_EQ(*result2, "hello, bye");
}

TEST_P(ReadableStreamTest, Close) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();
  ExceptionState& exception_state = scope.GetExceptionState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  auto* stream = ReadableStream::CreateWithCountQueueingStrategy(
      script_state, underlying_source, 0);

  ASSERT_TRUE(stream);

  EXPECT_EQ(stream->IsReadable(script_state, exception_state),
            base::make_optional(true));
  EXPECT_EQ(stream->IsClosed(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsErrored(script_state, exception_state),
            base::make_optional(false));

  underlying_source->Close();

  EXPECT_EQ(stream->IsReadable(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsClosed(script_state, exception_state),
            base::make_optional(true));
  EXPECT_EQ(stream->IsErrored(script_state, exception_state),
            base::make_optional(false));
}

TEST_P(ReadableStreamTest, Error) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();
  ExceptionState& exception_state = scope.GetExceptionState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  auto* stream = ReadableStream::CreateWithCountQueueingStrategy(
      script_state, underlying_source, 0);

  ASSERT_TRUE(stream);

  EXPECT_EQ(stream->IsReadable(script_state, exception_state),
            base::make_optional(true));
  EXPECT_EQ(stream->IsClosed(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsErrored(script_state, exception_state),
            base::make_optional(false));

  underlying_source->Error(
      ScriptValue(script_state, v8::Undefined(script_state->GetIsolate())));

  EXPECT_EQ(stream->IsReadable(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsClosed(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsErrored(script_state, exception_state),
            base::make_optional(true));
}

TEST_P(ReadableStreamTest, LockAndDisturb) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();
  ExceptionState& exception_state = scope.GetExceptionState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  auto* stream = ReadableStream::CreateWithCountQueueingStrategy(
      script_state, underlying_source, 0);

  ASSERT_TRUE(stream);

  EXPECT_EQ(stream->IsLocked(script_state, exception_state),
            base::make_optional(false));
  EXPECT_EQ(stream->IsDisturbed(script_state, exception_state),
            base::make_optional(false));

  stream->LockAndDisturb(script_state, exception_state);
  ASSERT_FALSE(exception_state.HadException());

  EXPECT_EQ(stream->IsLocked(script_state, exception_state),
            base::make_optional(true));
  EXPECT_EQ(stream->IsDisturbed(script_state, exception_state),
            base::make_optional(true));
}

TEST_P(ReadableStreamTest, Serialize) {
  if (GetParam()) {
    // Serialize() is not yet supported in the C++ implementation.
    return;
  }

  ScopedTransferableStreamsForTest enabled(true);
  RuntimeEnabledFeatures::SetTransferableStreamsEnabled(true);

  V8TestingScope scope;
  auto* script_state = scope.GetScriptState();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);
  auto* stream = ReadableStream::CreateWithCountQueueingStrategy(
      script_state, underlying_source, 0);
  ASSERT_TRUE(stream);

  MessageChannel* channel = MessageChannel::Create(scope.GetExecutionContext());

  stream->Serialize(script_state, channel->port1(), ASSERT_NO_EXCEPTION);
  EXPECT_TRUE(stream->IsLocked(script_state, ASSERT_NO_EXCEPTION));

  auto* transferred = ReadableStream::Deserialize(
      script_state, channel->port2(), ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(transferred);

  underlying_source->Enqueue(
      ScriptValue(script_state, V8String(script_state->GetIsolate(), "hello")));
  underlying_source->Enqueue(
      ScriptValue(script_state, V8String(script_state->GetIsolate(), ", bye")));
  underlying_source->Close();

  EXPECT_EQ(ReadAll(scope, transferred),
            base::make_optional<String>("hello, bye"));
}

TEST_P(ReadableStreamTest, GetReadHandle) {
  V8TestingScope scope;
  ScriptState* script_state = scope.GetScriptState();
  auto* isolate = scope.GetIsolate();

  auto* underlying_source =
      MakeGarbageCollected<TestUnderlyingSource>(script_state);

  ScriptValue js_underlying_source = ScriptValue(
      script_state,
      ToV8(underlying_source, script_state->GetContext()->Global(), isolate));
  ReadableStream* stream = ReadableStream::Create(
      script_state, js_underlying_source, ASSERT_NO_EXCEPTION);
  ASSERT_TRUE(stream);

  ScriptValue abc = ScriptValue(script_state, V8String(isolate, "abc"));
  underlying_source->Enqueue(abc);
  underlying_source->Close();

  ReadableStream::ReadHandle* read_handle =
      stream->GetReadHandle(script_state, ASSERT_NO_EXCEPTION);

  class CaptureFunction : public ScriptFunction {
   public:
    static v8::Local<v8::Function> CreateFunction(
        ScriptState* script_state,
        base::Optional<ScriptValue>* out_value) {
      return MakeGarbageCollected<CaptureFunction>(script_state, out_value)
          ->BindToV8Function();
    }

    CaptureFunction(ScriptState* script_state,
                    base::Optional<ScriptValue>* out_value)
        : ScriptFunction(script_state), out_value_(out_value) {}

    ScriptValue Call(ScriptValue value) override {
      *out_value_ = value;
      return value;
    }

   private:
    base::Optional<ScriptValue>* out_value_;
  };

  base::Optional<ScriptValue> iterator;
  base::Optional<ScriptValue> not_reached;

  read_handle->Read(script_state)
      .Then(CaptureFunction::CreateFunction(script_state, &iterator),
            CaptureFunction::CreateFunction(script_state, &not_reached));
  // Resolve promise and run callbacks.
  v8::MicrotasksScope::PerformCheckpoint(isolate);

  ASSERT_TRUE(iterator.has_value());
  EXPECT_FALSE(not_reached.has_value());
  ASSERT_FALSE(iterator->IsEmpty());

  bool done = false;
  auto maybe_value = V8UnpackIteratorResult(
      script_state, iterator->V8Value().As<v8::Object>(), &done);
  EXPECT_FALSE(done);
  auto value_as_string = ToBlinkString<String>(
      maybe_value.ToLocalChecked().As<v8::String>(), kDoNotExternalize);
  EXPECT_EQ(value_as_string, "abc");

  iterator.reset();
  read_handle->Read(script_state)
      .Then(CaptureFunction::CreateFunction(script_state, &iterator),
            CaptureFunction::CreateFunction(script_state, &not_reached));
  v8::MicrotasksScope::PerformCheckpoint(isolate);

  ASSERT_TRUE(iterator.has_value());
  EXPECT_FALSE(not_reached.has_value());
  ASSERT_FALSE(iterator->IsEmpty());

  maybe_value = V8UnpackIteratorResult(
      script_state, iterator->V8Value().As<v8::Object>(), &done);
  EXPECT_TRUE(done);
}

INSTANTIATE_TEST_SUITE_P(, ReadableStreamTest, ::testing::Values(false, true));

}  // namespace

}  // namespace blink

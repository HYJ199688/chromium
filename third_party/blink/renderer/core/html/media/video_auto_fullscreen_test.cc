// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/media/html_video_element.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"
#include "third_party/blink/renderer/core/html/media/autoplay_policy.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/testing/wait_for_event.h"
#include "third_party/blink/renderer/platform/testing/empty_web_media_player.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"

namespace blink {

class VideoAutoFullscreenFrameClient
    : public frame_test_helpers::TestWebFrameClient {
 public:
  WebMediaPlayer* CreateMediaPlayer(const WebMediaPlayerSource&,
                                    WebMediaPlayerClient*,
                                    WebMediaPlayerEncryptedMediaClient*,
                                    WebContentDecryptionModule*,
                                    const WebString& sink_id,
                                    WebLayerTreeView*) final {
    return new EmptyWebMediaPlayer();
  }

  void EnterFullscreen(const blink::WebFullscreenOptions&) final {
    Thread::Current()->GetTaskRunner()->PostTask(
        FROM_HERE,
        WTF::Bind(
            [](WebWidget* web_widget) { web_widget->DidEnterFullscreen(); },
            WTF::Unretained(web_widget_)));
  }

  void ExitFullscreen() final {
    Thread::Current()->GetTaskRunner()->PostTask(
        FROM_HERE,
        WTF::Bind(
            [](WebWidget* web_widget) { web_widget->DidExitFullscreen(); },
            WTF::Unretained(web_widget_)));
  }

  void set_frame_widget(WebWidget* web_widget) { web_widget_ = web_widget; }

 private:
  WebWidget* web_widget_;
};

class VideoAutoFullscreen : public testing::Test {
 public:
  void SetUp() override {
    RuntimeEnabledFeatures::SetVideoAutoFullscreenEnabled(true);

    web_view_helper_.Initialize(&web_frame_client_);
    GetWebView()->GetSettings()->SetAutoplayPolicy(
        WebSettings::AutoplayPolicy::kUserGestureRequired);

    frame_test_helpers::LoadFrame(
        web_view_helper_.GetWebView()->MainFrameImpl(), "about:blank");
    GetDocument()->write("<body><video></video></body>");

    video_ = ToHTMLVideoElement(*GetDocument()->QuerySelector("video"));

    web_frame_client_.set_frame_widget(GetWebView()->MainFrameWidget());
  }

  WebViewImpl* GetWebView() { return web_view_helper_.GetWebView(); }

  Document* GetDocument() {
    return web_view_helper_.GetWebView()->MainFrameImpl()->GetDocument();
  }

  LocalFrame* GetFrame() { return GetDocument()->GetFrame(); }

  HTMLVideoElement* Video() const { return video_.Get(); }

 private:
  Persistent<HTMLVideoElement> video_;
  VideoAutoFullscreenFrameClient web_frame_client_;
  frame_test_helpers::WebViewHelper web_view_helper_;
};

TEST_F(VideoAutoFullscreen, PlayTriggersFullscreenWithoutPlaysInline) {
  Video()->SetSrc("http://example.com/foo.mp4");

  std::unique_ptr<UserGestureIndicator> user_gesture_scope =
      LocalFrame::NotifyUserActivation(GetFrame(),
                                       UserGestureToken::kNewGesture);
  Video()->Play();

  WaitForEvent::Create(Video(), event_type_names::kPlay);
  test::RunPendingTasks();

  EXPECT_TRUE(Video()->IsFullscreen());
}

TEST_F(VideoAutoFullscreen, PlayDoesNotTriggerFullscreenWithPlaysInline) {
  Video()->SetBooleanAttribute(html_names::kPlaysinlineAttr, true);
  Video()->SetSrc("http://example.com/foo.mp4");

  std::unique_ptr<UserGestureIndicator> user_gesture_scope =
      LocalFrame::NotifyUserActivation(GetFrame(),
                                       UserGestureToken::kNewGesture);
  Video()->Play();

  WaitForEvent::Create(Video(), event_type_names::kPlay);
  test::RunPendingTasks();

  EXPECT_FALSE(Video()->IsFullscreen());
}

TEST_F(VideoAutoFullscreen, ExitFullscreenPausesWithoutPlaysInline) {
  Video()->SetSrc("http://example.com/foo.mp4");

  std::unique_ptr<UserGestureIndicator> user_gesture_scope =
      LocalFrame::NotifyUserActivation(GetFrame(),
                                       UserGestureToken::kNewGesture);
  Video()->Play();

  WaitForEvent::Create(Video(), event_type_names::kPlay);
  test::RunPendingTasks();
  ASSERT_TRUE(Video()->IsFullscreen());

  EXPECT_FALSE(Video()->paused());

  GetWebView()->ExitFullscreen(*GetFrame());
  test::RunPendingTasks();

  EXPECT_TRUE(Video()->paused());
}

TEST_F(VideoAutoFullscreen, ExitFullscreenDoesNotPauseWithPlaysInline) {
  Video()->SetBooleanAttribute(html_names::kPlaysinlineAttr, true);
  Video()->SetSrc("http://example.com/foo.mp4");

  std::unique_ptr<UserGestureIndicator> user_gesture_scope =
      LocalFrame::NotifyUserActivation(GetFrame(),
                                       UserGestureToken::kNewGesture);
  Video()->Play();

  WaitForEvent::Create(Video(), event_type_names::kPlay);
  Video()->webkitEnterFullscreen();
  test::RunPendingTasks();
  ASSERT_TRUE(Video()->IsFullscreen());

  EXPECT_FALSE(Video()->paused());

  GetWebView()->ExitFullscreen(*GetFrame());
  test::RunPendingTasks();

  EXPECT_FALSE(Video()->paused());
}

}  // namespace blink

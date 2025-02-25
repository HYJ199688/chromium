// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/environment.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/vr/test/mock_xr_device_hook_base.h"
#include "chrome/browser/vr/test/ui_utils.h"
#include "chrome/browser/vr/test/webxr_vr_browser_test.h"

#include <memory>

namespace vr {

namespace {

struct Frame {
  device_test::mojom::SubmittedFrameDataPtr submitted;
  device_test::mojom::PoseFrameDataPtr pose;
  device_test::mojom::DeviceConfigPtr config;
};

class MyXRMock : public MockXRDeviceHookBase {
 public:
  void OnFrameSubmitted(
      device_test::mojom::SubmittedFrameDataPtr frame_data,
      device_test::mojom::XRTestHook::OnFrameSubmittedCallback callback) final;
  void WaitGetDeviceConfig(
      device_test::mojom::XRTestHook::WaitGetDeviceConfigCallback callback)
      final {
    std::move(callback).Run(GetDeviceConfig());
  }
  void WaitGetPresentingPose(
      device_test::mojom::XRTestHook::WaitGetPresentingPoseCallback callback)
      final;
  void WaitGetMagicWindowPose(
      device_test::mojom::XRTestHook::WaitGetMagicWindowPoseCallback callback)
      final;

  // The test waits for a submitted frame before returning.
  void WaitForFrames(int count) {
    DCHECK(!wait_loop_);
    wait_frame_count_ = count;

    base::RunLoop* wait_loop =
        new base::RunLoop(base::RunLoop::Type::kNestableTasksAllowed);
    wait_loop_ = wait_loop;
    wait_loop->Run();
    delete wait_loop;
  }

  std::vector<Frame> submitted_frames;
  device_test::mojom::PoseFrameDataPtr last_immersive_frame_data;

  device_test::mojom::DeviceConfigPtr GetDeviceConfig() {
    auto config = device_test::mojom::DeviceConfig::New();
    config->interpupillary_distance = 0.2f;
    config->projection_left =
        device_test::mojom::ProjectionRaw::New(0.1f, 0.2f, 0.3f, 0.4f);
    config->projection_right =
        device_test::mojom::ProjectionRaw::New(0.5f, 0.6f, 0.7f, 0.8f);
    return config;
  }

 private:
  // Set to null on background thread after calling Quit(), so we can ensure we
  // only call Quit once.
  base::RunLoop* wait_loop_ = nullptr;

  int wait_frame_count_ = 0;
  int num_frames_submitted_ = 0;

  int frame_id_ = 0;
};

unsigned int ParseColorFrameId(const device_test::mojom::ColorPtr& color) {
  // Corresponding math in test_webxr_poses.html.
  unsigned int frame_id = static_cast<unsigned int>(color->r) + 256 * color->g +
                          256 * 256 * color->b;
  return frame_id;
}

void MyXRMock::OnFrameSubmitted(
    device_test::mojom::SubmittedFrameDataPtr frame_data,
    device_test::mojom::XRTestHook::OnFrameSubmittedCallback callback) {
  unsigned int frame_id = ParseColorFrameId(frame_data->color);
  DLOG(ERROR) << "Frame Submitted: " << num_frames_submitted_ << " "
              << frame_id;
  submitted_frames.push_back({std::move(frame_data),
                              last_immersive_frame_data.Clone(),
                              GetDeviceConfig()});

  num_frames_submitted_++;
  if (num_frames_submitted_ >= wait_frame_count_ && wait_frame_count_ > 0 &&
      wait_loop_) {
    wait_loop_->Quit();
    wait_loop_ = nullptr;
  }

  EXPECT_TRUE(!!last_immersive_frame_data)
      << "Frame submitted without any frame data provided";

  // We expect a waitGetPoses, then 2 submits (one for each eye), so after 2
  // submitted frames don't use the same frame_data again.
  if (num_frames_submitted_ % 2 == 0)
    last_immersive_frame_data = nullptr;

  std::move(callback).Run();
}

void MyXRMock::WaitGetMagicWindowPose(
    device_test::mojom::XRTestHook::WaitGetMagicWindowPoseCallback callback) {
  auto pose = device_test::mojom::PoseFrameData::New();

  // Almost identity matrix - enough different that we can identify if magic
  // window poses are used instead of presenting poses.
  pose->device_to_origin =
      gfx::Transform(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
  std::move(callback).Run(std::move(pose));
}

void MyXRMock::WaitGetPresentingPose(
    device_test::mojom::XRTestHook::WaitGetPresentingPoseCallback callback) {
  DLOG(ERROR) << "WaitGetPresentingPose: " << frame_id_;

  auto pose = device_test::mojom::PoseFrameData::New();

  // Start with identity matrix.
  pose->device_to_origin = gfx::Transform();

  // Add a translation so each frame gets a different transform, and so its easy
  // to identify what the expected pose is.
  pose->device_to_origin->Translate3d(0, 0, frame_id_);

  frame_id_++;
  last_immersive_frame_data = pose.Clone();

  std::move(callback).Run(std::move(pose));
}

std::string GetMatrixAsString(const gfx::Transform& m) {
  // Dump the transpose of the matrix due to openvr vs. webxr matrix format
  // differences.
  return base::StringPrintf(
      "[%f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f]",
      m.matrix().get(0, 0), m.matrix().get(1, 0), m.matrix().get(2, 0),
      m.matrix().get(3, 0), m.matrix().get(0, 1), m.matrix().get(1, 1),
      m.matrix().get(2, 1), m.matrix().get(3, 1), m.matrix().get(0, 2),
      m.matrix().get(1, 2), m.matrix().get(2, 2), m.matrix().get(3, 2),
      m.matrix().get(0, 3), m.matrix().get(1, 3), m.matrix().get(2, 3),
      m.matrix().get(3, 3));
}

std::string GetPoseAsString(const Frame& frame) {
  return GetMatrixAsString(*(frame.pose->device_to_origin));
}

}  // namespace

// Pixel test for WebVR/WebXR - start presentation, submit frames, get data back
// out. Validates that submitted frames used expected pose.
void TestPresentationPosesImpl(WebXrVrBrowserTestBase* t,
                               std::string filename) {
  // Disable frame-timeout UI to test what WebXR renders.
  UiUtils::DisableFrameTimeoutForTesting();
  MyXRMock my_mock;

  // Load the test page, and enter presentation.
  t->LoadUrlAndAwaitInitialization(t->GetFileUrlForHtmlTestFile(filename));
  t->EnterSessionWithUserGestureOrFail();

  // Wait for JavaScript to submit at least one frame.
  EXPECT_TRUE(
      t->PollJavaScriptBoolean("hasPresentedFrame", t->kPollTimeoutShort))
      << "No frame submitted";

  // Render at least 20 frames.  Make sure each has the right submitted pose.
  my_mock.WaitForFrames(20);

  // Exit presentation.
  t->EndSessionOrFail();

  // Stop hooking the VR runtime so we can safely analyze our cached data
  // without incoming calls (there may be leftover mojo messages queued).
  my_mock.StopHooking();

  // Analyze the submitted frames - check for a few things:
  // 1. Each frame id should be submitted at most once for each of the left and
  // right eyes.
  // 2. The pose that WebXR used for rendering the submitted frame should be the
  // one that we expected.
  std::set<unsigned int> seen_left;
  std::set<unsigned int> seen_right;
  unsigned int max_frame_id = 0;
  for (const auto& frame : my_mock.submitted_frames) {
    const device_test::mojom::SubmittedFrameDataPtr& data = frame.submitted;

    // The test page encodes the frame id as the clear color.
    unsigned int frame_id = ParseColorFrameId(data->color);

    // Validate that each frame is only seen once for each eye.
    DLOG(ERROR) << "Frame id: " << frame_id;
    if (data->eye == device_test::mojom::Eye::LEFT) {
      EXPECT_TRUE(seen_left.find(frame_id) == seen_left.end())
          << "Frame for left eye submitted more than once";
      seen_left.insert(frame_id);
    } else {
      EXPECT_TRUE(seen_right.find(frame_id) == seen_right.end())
          << "Frame for right eye submitted more than once";
      seen_right.insert(frame_id);
    }

    // Validate that frames arrive in order.
    EXPECT_TRUE(frame_id >= max_frame_id) << "Frame received out of order";
    max_frame_id = std::max(frame_id, max_frame_id);

    // Validate that the JavaScript-side cache of frames contains our submitted
    // frame.
    EXPECT_TRUE(t->RunJavaScriptAndExtractBoolOrFail(
        base::StringPrintf("checkFrameOccurred(%d)", frame_id)))
        << "JavaScript-side frame cache does not contain submitted frame";

    // Validate that the JavaScript-side cache of frames has the correct pose.
    EXPECT_TRUE(t->RunJavaScriptAndExtractBoolOrFail(base::StringPrintf(
        "checkFramePose(%d, %s)", frame_id, GetPoseAsString(frame).c_str())))
        << "JavaScript-side frame cache has incorrect pose";
  }

  // Tell JavaScript that it is done with the test.
  t->ExecuteStepAndWait("finishTest()");
  t->EndTest();
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard, TestPresentationPoses) {
  TestPresentationPosesImpl(this, "test_webxr_poses");
}

}  // namespace vr

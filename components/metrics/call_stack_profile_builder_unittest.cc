// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/call_stack_profile_builder.h"

#include <memory>

#include "base/files/file_path.h"
#include "base/sampling_heap_profiler/module_cache.h"
#include "base/test/bind_test_util.h"
#include "base/test/mock_callback.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/metrics/call_stack_profile_params.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/metrics_proto/sampled_profile.pb.h"

namespace metrics {

namespace {

using Frame = base::ProfileBuilder::Frame;

// Stub module for testing.
class TestModule : public base::ModuleCache::Module {
 public:
  TestModule(uintptr_t base_address = 0,
             const std::string& id = "",
             const base::FilePath& debug_basename = base::FilePath())
      : base_address_(base_address), id_(id), debug_basename_(debug_basename) {}

  TestModule(const TestModule&) = delete;
  TestModule& operator=(const TestModule&) = delete;

  uintptr_t GetBaseAddress() const override { return base_address_; }
  std::string GetId() const override { return id_; }
  base::FilePath GetDebugBasename() const override { return debug_basename_; }
  size_t GetSize() const override { return 0; }

 private:
  uintptr_t base_address_;
  std::string id_;
  base::FilePath debug_basename_;
};

constexpr CallStackProfileParams kProfileParams = {
    CallStackProfileParams::BROWSER_PROCESS,
    CallStackProfileParams::MAIN_THREAD,
    CallStackProfileParams::PROCESS_STARTUP};

class TestingCallStackProfileBuilder : public CallStackProfileBuilder {
 public:
  TestingCallStackProfileBuilder(
      const CallStackProfileParams& profile_params,
      const WorkIdRecorder* work_id_recorder = nullptr,
      const MetadataRecorder* metadata_recorder = nullptr,
      base::OnceClosure completed_callback = base::OnceClosure());

  ~TestingCallStackProfileBuilder() override;

  const SampledProfile& test_sampled_profile() { return test_sampled_profile_; }

 protected:
  // Overridden for testing.
  void PassProfilesToMetricsProvider(SampledProfile sampled_profile) override;

 private:
  // The completed profile.
  SampledProfile test_sampled_profile_;
};

TestingCallStackProfileBuilder::TestingCallStackProfileBuilder(
    const CallStackProfileParams& profile_params,
    const WorkIdRecorder* work_id_recorder,
    const MetadataRecorder* metadata_recorder,
    base::OnceClosure completed_callback)
    : CallStackProfileBuilder(profile_params,
                              work_id_recorder,
                              metadata_recorder,
                              std::move(completed_callback)) {}

TestingCallStackProfileBuilder::~TestingCallStackProfileBuilder() = default;

void TestingCallStackProfileBuilder::PassProfilesToMetricsProvider(
    SampledProfile sampled_profile) {
  test_sampled_profile_ = std::move(sampled_profile);
}

}  // namespace

TEST(CallStackProfileBuilderTest, ProfilingCompleted) {
  // Set up a mock completed callback which will be run once.
  base::MockCallback<base::OnceClosure> mock_closure;
  EXPECT_CALL(mock_closure, Run()).Times(1);

  auto profile_builder = std::make_unique<TestingCallStackProfileBuilder>(
      kProfileParams, nullptr, nullptr, mock_closure.Get());

#if defined(OS_WIN)
  uint64_t module_md5 = 0x46C3E4166659AC02ULL;
  base::FilePath module_path(L"c:\\some\\path\\to\\chrome.exe");
#else
  uint64_t module_md5 = 0x554838A8451AC36CULL;
  base::FilePath module_path("/some/path/to/chrome");
#endif

  const uintptr_t module_base_address1 = 0x1000;
  TestModule module1(module_base_address1, "1", module_path);
  Frame frame1 = {module_base_address1 + 0x10, &module1};

  const uintptr_t module_base_address2 = 0x1100;
  TestModule module2(module_base_address2, "2", module_path);
  Frame frame2 = {module_base_address2 + 0x10, &module2};

  const uintptr_t module_base_address3 = 0x1010;
  TestModule module3(module_base_address3, "3", module_path);
  Frame frame3 = {module_base_address3 + 0x10, &module3};

  std::vector<Frame> frames1 = {frame1, frame2};
  std::vector<Frame> frames2 = {frame3};

  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames1);
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames2);
  profile_builder->OnProfileCompleted(base::TimeDelta::FromMilliseconds(500),
                                      base::TimeDelta::FromMilliseconds(100));

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_process());
  ASSERT_EQ(BROWSER_PROCESS, proto.process());
  ASSERT_TRUE(proto.has_thread());
  ASSERT_EQ(MAIN_THREAD, proto.thread());
  ASSERT_TRUE(proto.has_trigger_event());
  ASSERT_EQ(SampledProfile::PROCESS_STARTUP, proto.trigger_event());

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();

  ASSERT_EQ(2, profile.stack_size());
  ASSERT_EQ(2, profile.stack(0).frame_size());
  ASSERT_TRUE(profile.stack(0).frame(0).has_module_id_index());
  EXPECT_EQ(0, profile.stack(0).frame(0).module_id_index());
  ASSERT_TRUE(profile.stack(0).frame(1).has_module_id_index());
  EXPECT_EQ(1, profile.stack(0).frame(1).module_id_index());
  ASSERT_EQ(1, profile.stack(1).frame_size());
  ASSERT_TRUE(profile.stack(1).frame(0).has_module_id_index());
  EXPECT_EQ(2, profile.stack(1).frame(0).module_id_index());

  ASSERT_EQ(3, profile.module_id().size());
  ASSERT_TRUE(profile.module_id(0).has_build_id());
  EXPECT_EQ("1", profile.module_id(0).build_id());
  ASSERT_TRUE(profile.module_id(0).has_name_md5_prefix());
  EXPECT_EQ(module_md5, profile.module_id(0).name_md5_prefix());
  ASSERT_TRUE(profile.module_id(1).has_build_id());
  EXPECT_EQ("2", profile.module_id(1).build_id());
  ASSERT_TRUE(profile.module_id(1).has_name_md5_prefix());
  EXPECT_EQ(module_md5, profile.module_id(1).name_md5_prefix());
  ASSERT_TRUE(profile.module_id(2).has_build_id());
  EXPECT_EQ("3", profile.module_id(2).build_id());
  ASSERT_TRUE(profile.module_id(2).has_name_md5_prefix());
  EXPECT_EQ(module_md5, profile.module_id(2).name_md5_prefix());

  ASSERT_EQ(2, profile.stack_sample_size());
  EXPECT_EQ(0, profile.stack_sample(0).stack_index());
  EXPECT_FALSE(profile.stack_sample(0).has_continued_work());
  EXPECT_EQ(1, profile.stack_sample(1).stack_index());
  EXPECT_FALSE(profile.stack_sample(1).has_continued_work());

  ASSERT_TRUE(profile.has_profile_duration_ms());
  EXPECT_EQ(500, profile.profile_duration_ms());
  ASSERT_TRUE(profile.has_sampling_period_ms());
  EXPECT_EQ(100, profile.sampling_period_ms());
}

TEST(CallStackProfileBuilderTest, StacksDeduped) {
  auto profile_builder =
      std::make_unique<TestingCallStackProfileBuilder>(kProfileParams);

  TestModule module1;
  Frame frame1 = {0x10, &module1};

  TestModule module2;
  Frame frame2 = {0x20, &module2};

  std::vector<Frame> frames = {frame1, frame2};

  // Two stacks are completed with the same frames therefore they are deduped
  // to one.
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames);
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames);

  profile_builder->OnProfileCompleted(base::TimeDelta(), base::TimeDelta());

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_process());
  ASSERT_EQ(BROWSER_PROCESS, proto.process());
  ASSERT_TRUE(proto.has_thread());
  ASSERT_EQ(MAIN_THREAD, proto.thread());
  ASSERT_TRUE(proto.has_trigger_event());
  ASSERT_EQ(SampledProfile::PROCESS_STARTUP, proto.trigger_event());

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();
  ASSERT_EQ(1, profile.stack_size());
  ASSERT_EQ(2, profile.stack_sample_size());
  EXPECT_EQ(0, profile.stack_sample(0).stack_index());
  EXPECT_EQ(0, profile.stack_sample(1).stack_index());
}

TEST(CallStackProfileBuilderTest, StacksNotDeduped) {
  auto profile_builder =
      std::make_unique<TestingCallStackProfileBuilder>(kProfileParams);

  TestModule module1;
  Frame frame1 = {0x10, &module1};

  TestModule module2;
  Frame frame2 = {0x20, &module2};

  std::vector<Frame> frames1 = {frame1};
  std::vector<Frame> frames2 = {frame2};

  // Two stacks are completed with the different frames therefore not deduped.
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames1);
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames2);

  profile_builder->OnProfileCompleted(base::TimeDelta(), base::TimeDelta());

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_process());
  ASSERT_EQ(BROWSER_PROCESS, proto.process());
  ASSERT_TRUE(proto.has_thread());
  ASSERT_EQ(MAIN_THREAD, proto.thread());
  ASSERT_TRUE(proto.has_trigger_event());
  ASSERT_EQ(SampledProfile::PROCESS_STARTUP, proto.trigger_event());

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();
  ASSERT_EQ(2, profile.stack_size());
  ASSERT_EQ(2, profile.stack_sample_size());
  EXPECT_EQ(0, profile.stack_sample(0).stack_index());
  EXPECT_EQ(1, profile.stack_sample(1).stack_index());
}

TEST(CallStackProfileBuilderTest, Modules) {
  auto profile_builder =
      std::make_unique<TestingCallStackProfileBuilder>(kProfileParams);

  // A frame with no module.
  Frame frame1 = {0x1010, nullptr};

  const uintptr_t module_base_address2 = 0x1100;
#if defined(OS_WIN)
  uint64_t module_md5 = 0x46C3E4166659AC02ULL;
  base::FilePath module_path(L"c:\\some\\path\\to\\chrome.exe");
#else
  uint64_t module_md5 = 0x554838A8451AC36CULL;
  base::FilePath module_path("/some/path/to/chrome");
#endif
  TestModule module2(module_base_address2, "2", module_path);
  Frame frame2 = {module_base_address2 + 0x10, &module2};

  std::vector<Frame> frames = {frame1, frame2};

  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames);
  profile_builder->OnProfileCompleted(base::TimeDelta(), base::TimeDelta());

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();

  ASSERT_EQ(1, profile.stack_sample_size());
  EXPECT_EQ(0, profile.stack_sample(0).stack_index());

  ASSERT_EQ(1, profile.stack_size());
  ASSERT_EQ(2, profile.stack(0).frame_size());

  ASSERT_FALSE(profile.stack(0).frame(0).has_module_id_index());
  ASSERT_FALSE(profile.stack(0).frame(0).has_address());

  ASSERT_TRUE(profile.stack(0).frame(1).has_module_id_index());
  EXPECT_EQ(0, profile.stack(0).frame(1).module_id_index());
  ASSERT_TRUE(profile.stack(0).frame(1).has_address());
  EXPECT_EQ(0x10ULL, profile.stack(0).frame(1).address());

  ASSERT_EQ(1, profile.module_id().size());
  ASSERT_TRUE(profile.module_id(0).has_build_id());
  EXPECT_EQ("2", profile.module_id(0).build_id());
  ASSERT_TRUE(profile.module_id(0).has_name_md5_prefix());
  EXPECT_EQ(module_md5, profile.module_id(0).name_md5_prefix());
}

TEST(CallStackProfileBuilderTest, DedupModules) {
  auto profile_builder =
      std::make_unique<TestingCallStackProfileBuilder>(kProfileParams);

  const uintptr_t module_base_address = 0x1000;

#if defined(OS_WIN)
  uint64_t module_md5 = 0x46C3E4166659AC02ULL;
  base::FilePath module_path(L"c:\\some\\path\\to\\chrome.exe");
#else
  uint64_t module_md5 = 0x554838A8451AC36CULL;
  base::FilePath module_path("/some/path/to/chrome");
#endif

  TestModule module(module_base_address, "1", module_path);
  Frame frame1 = {module_base_address + 0x10, &module};
  Frame frame2 = {module_base_address + 0x20, &module};

  std::vector<Frame> frames = {frame1, frame2};

  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted(frames);
  profile_builder->OnProfileCompleted(base::TimeDelta(), base::TimeDelta());

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();

  ASSERT_EQ(1, profile.stack_sample_size());
  EXPECT_EQ(0, profile.stack_sample(0).stack_index());

  ASSERT_EQ(1, profile.stack_size());
  ASSERT_EQ(2, profile.stack(0).frame_size());

  // The two frames share the same module, which should be deduped in the
  // output.
  ASSERT_TRUE(profile.stack(0).frame(0).has_module_id_index());
  EXPECT_EQ(0, profile.stack(0).frame(0).module_id_index());
  ASSERT_TRUE(profile.stack(0).frame(0).has_address());
  EXPECT_EQ(0x10ULL, profile.stack(0).frame(0).address());

  ASSERT_TRUE(profile.stack(0).frame(1).has_module_id_index());
  EXPECT_EQ(0, profile.stack(0).frame(1).module_id_index());
  ASSERT_TRUE(profile.stack(0).frame(1).has_address());
  EXPECT_EQ(0x20ULL, profile.stack(0).frame(1).address());

  ASSERT_EQ(1, profile.module_id().size());
  ASSERT_TRUE(profile.module_id(0).has_build_id());
  EXPECT_EQ("1", profile.module_id(0).build_id());
  ASSERT_TRUE(profile.module_id(0).has_name_md5_prefix());
  EXPECT_EQ(module_md5, profile.module_id(0).name_md5_prefix());
}

TEST(CallStackProfileBuilderTest, WorkIds) {
  class TestWorkIdRecorder : public WorkIdRecorder {
   public:
    unsigned int RecordWorkId() const override { return current_id; }

    unsigned int current_id = 0;
  };

  TestWorkIdRecorder work_id_recorder;
  auto profile_builder = std::make_unique<TestingCallStackProfileBuilder>(
      kProfileParams, &work_id_recorder);

  TestModule module;
  Frame frame = {0x10, &module};

  // Id 0 means the message loop hasn't been started yet, so the sample should
  // not have continued_work set.
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted({frame});

  // The second sample with the same id should have continued_work set.
  work_id_recorder.current_id = 1;
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted({frame});
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted({frame});

  // Ids are in general non-contiguous across multiple samples.
  work_id_recorder.current_id = 10;
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted({frame});
  profile_builder->RecordMetadata();
  profile_builder->OnSampleCompleted({frame});

  profile_builder->OnProfileCompleted(base::TimeDelta::FromMilliseconds(500),
                                      base::TimeDelta::FromMilliseconds(100));

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();

  ASSERT_EQ(5, profile.stack_sample_size());
  EXPECT_FALSE(profile.stack_sample(0).has_continued_work());
  EXPECT_FALSE(profile.stack_sample(1).has_continued_work());
  EXPECT_TRUE(profile.stack_sample(2).continued_work());
  EXPECT_FALSE(profile.stack_sample(3).has_continued_work());
  EXPECT_TRUE(profile.stack_sample(4).continued_work());
}

TEST(CallStackProfileBuilderTest, MetadataRecorder) {
  class TestMetadataRecorder : public MetadataRecorder {
   public:
    std::pair<uint64_t, int64_t> GetHashAndValue() const override {
      return std::make_pair(0x5A105E8B9D40E132ull, current_value);
    }
    int64_t current_value;
  };

  TestMetadataRecorder metadata_recorder;
  auto profile_builder = std::make_unique<TestingCallStackProfileBuilder>(
      kProfileParams, nullptr, &metadata_recorder);

  TestModule module;
  Frame frame = {0x10, &module};

  metadata_recorder.current_value = 5;
  profile_builder->OnSampleCompleted({frame});
  metadata_recorder.current_value = 8;
  profile_builder->OnSampleCompleted({frame});

  profile_builder->OnProfileCompleted(base::TimeDelta::FromMilliseconds(500),
                                      base::TimeDelta::FromMilliseconds(100));

  const SampledProfile& proto = profile_builder->test_sampled_profile();

  ASSERT_TRUE(proto.has_call_stack_profile());
  const CallStackProfile& profile = proto.call_stack_profile();

  ASSERT_EQ(2, profile.stack_sample_size());
  EXPECT_EQ(1, profile.stack_sample(0).metadata_size());
  EXPECT_EQ(1, profile.stack_sample(1).metadata_size());

  ASSERT_EQ(1, profile.metadata_name_hash_size());
  EXPECT_EQ(0x5A105E8B9D40E132ull, profile.metadata_name_hash(0));

  EXPECT_EQ(0, profile.stack_sample(0).metadata(0).name_hash_index());
  EXPECT_EQ(5, profile.stack_sample(0).metadata(0).value());
  EXPECT_EQ(0, profile.stack_sample(1).metadata(0).name_hash_index());
  EXPECT_EQ(8, profile.stack_sample(1).metadata(0).value());
}

}  // namespace metrics

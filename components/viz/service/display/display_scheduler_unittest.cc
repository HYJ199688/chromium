// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display/display_scheduler.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/test/null_task_runner.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/trace_event/trace_event.h"
#include "cc/test/scheduler_test_common.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/surfaces/surface_info.h"
#include "components/viz/service/display/display.h"
#include "components/viz/test/begin_frame_args_test.h"
#include "components/viz/test/fake_external_begin_frame_source.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace viz {
namespace {

const int kMaxPendingSwaps = 1;

static constexpr FrameSinkId kArbitraryFrameSinkId(1, 1);

class FakeDisplaySchedulerClient : public DisplaySchedulerClient {
 public:
  FakeDisplaySchedulerClient()
      : draw_and_swap_count_(0), next_draw_and_swap_fails_(false) {}

  ~FakeDisplaySchedulerClient() override {}

  bool DrawAndSwap() override {
    draw_and_swap_count_++;

    bool success = !next_draw_and_swap_fails_;
    next_draw_and_swap_fails_ = false;

    if (success)
      undrawn_surfaces_.clear();
    return success;
  }

  bool SurfaceHasUnackedFrame(const SurfaceId& surface_id) const override {
    return base::ContainsKey(undrawn_surfaces_, surface_id);
  }

  bool SurfaceDamaged(const SurfaceId& surface_id,
                      const BeginFrameAck& ack) override {
    return false;
  }

  void SurfaceDiscarded(const SurfaceId& surface_id) override {}

  void DidFinishFrame(const BeginFrameAck& ack) override {
    last_begin_frame_ack_ = ack;
  }

  int draw_and_swap_count() const { return draw_and_swap_count_; }

  void SetNextDrawAndSwapFails() { next_draw_and_swap_fails_ = true; }

  void SurfaceDamaged(const SurfaceId& surface_id) {
    undrawn_surfaces_.insert(surface_id);
  }

  const BeginFrameAck& last_begin_frame_ack() { return last_begin_frame_ack_; }

 protected:
  int draw_and_swap_count_;
  bool next_draw_and_swap_fails_;
  std::set<SurfaceId> undrawn_surfaces_;
  BeginFrameAck last_begin_frame_ack_;
};

class TestDisplayScheduler : public DisplayScheduler {
 public:
  TestDisplayScheduler(BeginFrameSource* begin_frame_source,
                       SurfaceManager* surface_manager,
                       base::SingleThreadTaskRunner* task_runner,
                       int max_pending_swaps,
                       bool wait_for_all_surfaces_before_draw)
      : DisplayScheduler(begin_frame_source,
                         task_runner,
                         max_pending_swaps,
                         wait_for_all_surfaces_before_draw),
        scheduler_begin_frame_deadline_count_(0) {}

  base::TimeTicks DesiredBeginFrameDeadlineTimeForTest() {
    return DesiredBeginFrameDeadlineTime();
  }

  void BeginFrameDeadlineForTest() {
    // Ensure that any missed BeginFrames were handled by the scheduler. We need
    // to run the scheduled task ourselves since the NullTaskRunner won't.
    if (!missed_begin_frame_task_.IsCancelled())
      missed_begin_frame_task_.callback().Run();
    OnBeginFrameDeadline();
  }

  void ScheduleBeginFrameDeadline() override {
    scheduler_begin_frame_deadline_count_++;
    DisplayScheduler::ScheduleBeginFrameDeadline();
  }

  int scheduler_begin_frame_deadline_count() {
    return scheduler_begin_frame_deadline_count_;
  }

  bool inside_begin_frame_deadline_interval() {
    return inside_begin_frame_deadline_interval_;
  }

  bool has_pending_surfaces() { return has_pending_surfaces_; }

 protected:
  int scheduler_begin_frame_deadline_count_;
};

class DisplaySchedulerTest : public testing::Test {
 public:
  explicit DisplaySchedulerTest(bool wait_for_all_surfaces_before_draw = false)
      : fake_begin_frame_source_(0.f, false),
        task_runner_(new base::NullTaskRunner),
        surface_manager_(nullptr, 4u),
        scheduler_(&fake_begin_frame_source_,
                   &surface_manager_,
                   task_runner_.get(),
                   kMaxPendingSwaps,
                   wait_for_all_surfaces_before_draw) {
    now_src_.Advance(base::TimeDelta::FromMicroseconds(10000));
    surface_manager_.AddObserver(&scheduler_);
    scheduler_.SetClient(&client_);
  }

  ~DisplaySchedulerTest() override {
    surface_manager_.RemoveObserver(&scheduler_);
  }

  void SetUp() override { scheduler_.SetRootFrameMissing(false); }

  void AdvanceTimeAndBeginFrameForTest(
      const std::vector<SurfaceId>& observing_surfaces) {
    now_src_.Advance(base::TimeDelta::FromMicroseconds(10000));
    // FakeBeginFrameSource deals with |source_id| and |sequence_number|.
    last_begin_frame_args_ = fake_begin_frame_source_.CreateBeginFrameArgs(
        BEGINFRAME_FROM_HERE, &now_src_);
    fake_begin_frame_source_.TestOnBeginFrame(last_begin_frame_args_);
    for (const auto& surface_id : observing_surfaces)
      scheduler_.OnSurfaceDamageExpected(surface_id, last_begin_frame_args_);
  }

  void SurfaceDamaged(const SurfaceId& surface_id) {
    client_.SurfaceDamaged(surface_id);
    scheduler_.ProcessSurfaceDamage(surface_id, AckForCurrentBeginFrame(),
                                    true);
  }

 protected:
  base::SimpleTestTickClock& now_src() { return now_src_; }
  FakeDisplaySchedulerClient& client() { return client_; }
  DisplayScheduler& scheduler() { return scheduler_; }
  BeginFrameAck AckForCurrentBeginFrame() {
    DCHECK(last_begin_frame_args_.IsValid());
    return BeginFrameAck(last_begin_frame_args_, true);
  }

  FakeExternalBeginFrameSource fake_begin_frame_source_;
  BeginFrameArgs last_begin_frame_args_;

  base::SimpleTestTickClock now_src_;
  scoped_refptr<base::NullTaskRunner> task_runner_;
  SurfaceManager surface_manager_;
  FakeDisplaySchedulerClient client_;
  TestDisplayScheduler scheduler_;
};

TEST_F(DisplaySchedulerTest, ResizeHasLateDeadlineUntilNewRootSurface) {
  SurfaceId root_surface_id1(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId root_surface_id2(
      kArbitraryFrameSinkId,
      LocalSurfaceId(2, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(3, base::UnguessableToken::Create()));
  base::TimeTicks late_deadline;

  scheduler_.SetVisible(true);

  // Go trough an initial BeginFrame cycle with the root surface.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  scheduler_.SetNewRootSurface(root_surface_id1);
  scheduler_.BeginFrameDeadlineForTest();

  // Resize on the next begin frame cycle should cause the deadline to wait
  // for a new root surface.
  AdvanceTimeAndBeginFrameForTest({root_surface_id1});
  late_deadline = now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  SurfaceDamaged(sid1);
  EXPECT_GT(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.DisplayResized();
  EXPECT_EQ(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.OnSurfaceDestroyed(root_surface_id1);
  scheduler_.SetNewRootSurface(root_surface_id2);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // Verify deadline goes back to normal after resize.
  late_deadline = now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  AdvanceTimeAndBeginFrameForTest({root_surface_id2, sid1});
  SurfaceDamaged(sid1);
  EXPECT_GT(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(root_surface_id2);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
}

TEST_F(DisplaySchedulerTest, ResizeHasLateDeadlineUntilDamagedSurface) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  base::TimeTicks late_deadline;

  scheduler_.SetVisible(true);

  // Go trough an initial BeginFrame cycle with the root surface.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  scheduler_.SetNewRootSurface(root_surface_id);
  scheduler_.BeginFrameDeadlineForTest();

  // Resize on the next begin frame cycle should cause the deadline to wait
  // for a new root surface.
  AdvanceTimeAndBeginFrameForTest({root_surface_id});
  late_deadline = now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  SurfaceDamaged(sid1);
  EXPECT_GT(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.DisplayResized();
  EXPECT_EQ(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(root_surface_id);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // Verify deadline goes back to normal after resize.
  AdvanceTimeAndBeginFrameForTest({root_surface_id, sid1});
  late_deadline = now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  SurfaceDamaged(sid1);
  EXPECT_GT(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(root_surface_id);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
}

TEST_F(DisplaySchedulerTest, SurfaceDamaged) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  SurfaceId sid2(kArbitraryFrameSinkId,
                 LocalSurfaceId(3, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);
  EXPECT_EQ(BeginFrameAck(), client_.last_begin_frame_ack());

  // Set surface1 as active via SurfaceDamageExpected().
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_EQ(BeginFrameAck(), client_.last_begin_frame_ack());

  // Damage only from surface 2 (inactive) does not trigger deadline early.
  SurfaceDamaged(sid2);
  EXPECT_TRUE(scheduler_.has_pending_surfaces());
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Damage from surface 1 triggers deadline early.
  SurfaceDamaged(sid1);
  EXPECT_FALSE(scheduler_.has_pending_surfaces());
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(BeginFrameAck(last_begin_frame_args_, true),
            client_.last_begin_frame_ack());

  // Set both surface 1 and 2 as active via SurfaceDamageExpected().
  AdvanceTimeAndBeginFrameForTest({sid1, sid2});

  // Deadline doesn't trigger early until surface 1 and 2 are both damaged.
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(sid1);
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(sid2);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(BeginFrameAck(last_begin_frame_args_, true),
            client_.last_begin_frame_ack());

  // Surface damage with |!has_damage| triggers early deadline if other damage
  // exists.
  AdvanceTimeAndBeginFrameForTest({sid1, sid2});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(sid2);
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  BeginFrameAck ack = AckForCurrentBeginFrame();
  ack.has_damage = false;
  scheduler_.ProcessSurfaceDamage(sid1, ack, false);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // Surface damage with |!has_damage| does not trigger early deadline if no
  // other damage exists.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  ack = AckForCurrentBeginFrame();
  ack.has_damage = false;
  scheduler_.ProcessSurfaceDamage(sid1, ack, false);
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(BeginFrameAck(last_begin_frame_args_, false),
            client_.last_begin_frame_ack());

  // System should be idle now.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());

  // Surface damage with |!display_damaged| does not affect needs_draw and
  // scheduler stays idle.
  scheduler_.ProcessSurfaceDamage(sid1, AckForCurrentBeginFrame(), false);
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());

  // Deadline should trigger early if child surfaces are idle and
  // we get damage on the root surface.
  scheduler_.OnSurfaceDamageExpected(root_surface_id, last_begin_frame_args_);
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());
  SurfaceDamaged(root_surface_id);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
}

class DisplaySchedulerWaitForAllSurfacesTest : public DisplaySchedulerTest {
 public:
  DisplaySchedulerWaitForAllSurfacesTest()
      : DisplaySchedulerTest(true /* wait_for_all_surfaces_before_draw */) {}
};

TEST_F(DisplaySchedulerWaitForAllSurfacesTest, WaitForAllSurfacesBeforeDraw) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  SurfaceId sid2(kArbitraryFrameSinkId,
                 LocalSurfaceId(3, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // Set surface1 as active via SurfaceDamageExpected().
  AdvanceTimeAndBeginFrameForTest({sid1});

  // Deadline is blocked indefinitely until surface 1 is damaged.
  EXPECT_EQ(base::TimeTicks::Max(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Damage only from surface 2 (inactive) does not change deadline.
  SurfaceDamaged(sid2);
  EXPECT_TRUE(scheduler_.has_pending_surfaces());
  EXPECT_EQ(base::TimeTicks::Max(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Damage from surface 1 triggers deadline immediately.
  SurfaceDamaged(sid1);
  EXPECT_FALSE(scheduler_.has_pending_surfaces());
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // Surface damage with |!has_damage| triggers immediate deadline if other
  // damage exists.
  AdvanceTimeAndBeginFrameForTest({sid1, sid2});
  EXPECT_EQ(base::TimeTicks::Max(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  SurfaceDamaged(sid2);
  EXPECT_EQ(base::TimeTicks::Max(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  BeginFrameAck ack = AckForCurrentBeginFrame();
  ack.has_damage = false;
  scheduler_.ProcessSurfaceDamage(sid1, ack, false);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // Surface damage with |!has_damage| also triggers immediate deadline even if
  // no other damage exists.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_EQ(base::TimeTicks::Max(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  ack = AckForCurrentBeginFrame();
  ack.has_damage = false;
  scheduler_.ProcessSurfaceDamage(sid1, ack, false);
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  // Stray BeginFrameAcks for older BeginFrames are ignored.
  ack.sequence_number--;
  scheduler_.ProcessSurfaceDamage(sid1, ack, false);
  // If the acknowledgment above was not ignored and instead updated the surface
  // state for sid1, the surface would become a pending surface again, and the
  // deadline would no longer be immediate. Since it is ignored, we are
  // expecting the deadline to remain immedate.
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();

  // System should be idle now because we had a frame without damage. Restore it
  // to active state (DisplayScheduler observing BeginFrames) for the next test.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();

  // BeginFrame without expected surface damage triggers immediate deadline.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_TRUE(scheduler_.inside_begin_frame_deadline_interval());
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.BeginFrameDeadlineForTest();
}

TEST_F(DisplaySchedulerTest, OutputSurfaceLost) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // DrawAndSwap normally.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  EXPECT_EQ(0, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  // Deadline triggers immediately on OutputSurfaceLost.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.OutputSurfaceLost();
  EXPECT_GE(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Deadline does not DrawAndSwap after OutputSurfaceLost.
  EXPECT_EQ(1, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());
}

TEST_F(DisplaySchedulerTest, VisibleWithoutDamageNoTicks) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));

  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());
  scheduler_.SetVisible(true);

  // When becoming visible, don't start listening for begin frames until there
  // is some damage.
  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());
  scheduler_.SetNewRootSurface(root_surface_id);

  EXPECT_EQ(1u, fake_begin_frame_source_.num_observers());
}

TEST_F(DisplaySchedulerTest, VisibleWithDamageTicks) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));

  scheduler_.SetNewRootSurface(root_surface_id);

  // When there is damage, start listening for begin frames once becoming
  // visible.
  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());
  scheduler_.SetVisible(true);

  EXPECT_EQ(1u, fake_begin_frame_source_.num_observers());
}

TEST_F(DisplaySchedulerTest, Visibility) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));

  // Set the root surface.
  scheduler_.SetNewRootSurface(root_surface_id);
  scheduler_.SetVisible(true);
  EXPECT_EQ(1u, fake_begin_frame_source_.num_observers());

  // DrawAndSwap normally.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  EXPECT_EQ(0, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Become not visible.
  scheduler_.SetVisible(false);

  // It will stop listening for begin frames after the current deadline.
  EXPECT_EQ(1u, fake_begin_frame_source_.num_observers());

  // Deadline does not DrawAndSwap when not visible.
  EXPECT_EQ(1, client_.draw_and_swap_count());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());
  // Now it stops listening for begin frames.
  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());

  // Does not start listening for begin frames when becoming visible without
  // damage.
  scheduler_.SetVisible(true);
  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());
  scheduler_.SetVisible(false);

  // Does not start listening for begin frames when damage arrives.
  SurfaceDamaged(sid1);
  EXPECT_EQ(0u, fake_begin_frame_source_.num_observers());

  // But does when becoming visible with damage again.
  scheduler_.SetVisible(true);
  EXPECT_EQ(1u, fake_begin_frame_source_.num_observers());
}

TEST_F(DisplaySchedulerTest, ResizeCausesSwap) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // DrawAndSwap normally.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  EXPECT_EQ(0, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  scheduler_.DisplayResized();
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  // DisplayResized should trigger a swap to happen.
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(2, client_.draw_and_swap_count());
}

TEST_F(DisplaySchedulerTest, RootFrameMissing) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  base::TimeTicks late_deadline;

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // DrawAndSwap normally.
  AdvanceTimeAndBeginFrameForTest({sid1});
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  EXPECT_EQ(0, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  // Deadline triggers late while root frame is missing.
  AdvanceTimeAndBeginFrameForTest({sid1});
  late_deadline = now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  SurfaceDamaged(sid1);
  EXPECT_GT(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.SetRootFrameMissing(true);
  EXPECT_EQ(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Deadline does not DrawAndSwap while root frame is missing.
  EXPECT_EQ(1, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  //  Deadline triggers normally when root frame is not missing.
  AdvanceTimeAndBeginFrameForTest({sid1, root_surface_id});
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());
  SurfaceDamaged(sid1);

  // The deadline is not updated because the display scheduler does not receive
  // a BeginFrame while the root frame is missing.
  EXPECT_EQ(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  scheduler_.SetRootFrameMissing(false);
  EXPECT_TRUE(scheduler_.inside_begin_frame_deadline_interval());
  SurfaceDamaged(root_surface_id);
  EXPECT_EQ(base::TimeTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  EXPECT_EQ(1, client_.draw_and_swap_count());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(2, client_.draw_and_swap_count());
}

TEST_F(DisplaySchedulerTest, DidSwapBuffers) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  SurfaceId sid2(kArbitraryFrameSinkId,
                 LocalSurfaceId(3, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // Set surface 1 and 2 as active.
  AdvanceTimeAndBeginFrameForTest({sid1, sid2});

  // DrawAndSwap normally.
  EXPECT_LT(now_src().NowTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  EXPECT_EQ(0, client_.draw_and_swap_count());
  SurfaceDamaged(sid1);
  SurfaceDamaged(sid2);
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());
  scheduler_.DidSwapBuffers();

  // Deadline triggers late when swap throttled.
  AdvanceTimeAndBeginFrameForTest({sid1, sid2});
  base::TimeTicks late_deadline =
      now_src().NowTicks() + BeginFrameArgs::DefaultInterval();
  // Damage surface 1, but not surface 2 so we avoid triggering deadline
  // early because all surfaces are ready.
  SurfaceDamaged(sid1);
  EXPECT_EQ(late_deadline, scheduler_.DesiredBeginFrameDeadlineTimeForTest());

  // Don't draw and swap in deadline while swap throttled.
  EXPECT_EQ(1, client_.draw_and_swap_count());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(1, client_.draw_and_swap_count());

  // Deadline triggers normally once not swap throttled.
  // Damage from previous BeginFrame should cary over, so don't damage again.
  scheduler_.DidReceiveSwapBuffersAck();
  AdvanceTimeAndBeginFrameForTest({sid2});
  base::TimeTicks expected_deadline =
      scheduler_.LastUsedBeginFrameArgs().deadline -
      BeginFrameArgs::DefaultEstimatedDisplayDrawTime(
          scheduler_.LastUsedBeginFrameArgs().interval);
  EXPECT_EQ(expected_deadline,
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  // Still waiting for surface 2. Once it updates, deadline should trigger
  // immediately again.
  SurfaceDamaged(sid2);
  EXPECT_EQ(base::TimeTicks(),
            scheduler_.DesiredBeginFrameDeadlineTimeForTest());
  // Draw and swap now that we aren't throttled.
  EXPECT_EQ(1, client_.draw_and_swap_count());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(2, client_.draw_and_swap_count());
}

// This test verfies that we try to reschedule the deadline
// after any event that may change what deadline we want.
TEST_F(DisplaySchedulerTest, ScheduleBeginFrameDeadline) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));
  SurfaceId sid1(kArbitraryFrameSinkId,
                 LocalSurfaceId(2, base::UnguessableToken::Create()));
  int count = 1;
  EXPECT_EQ(count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetVisible(true);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetVisible(true);
  EXPECT_EQ(count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetVisible(false);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  // Set the root surface while not visible.
  scheduler_.SetNewRootSurface(root_surface_id);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetVisible(true);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  // Set the root surface while visible.
  scheduler_.SetNewRootSurface(root_surface_id);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.BeginFrameDeadlineForTest();
  scheduler_.DidSwapBuffers();
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.DidReceiveSwapBuffersAck();
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.DisplayResized();
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetNewRootSurface(root_surface_id);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  SurfaceDamaged(sid1);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.SetRootFrameMissing(true);
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());

  scheduler_.OutputSurfaceLost();
  EXPECT_EQ(++count, scheduler_.scheduler_begin_frame_deadline_count());
}

TEST_F(DisplaySchedulerTest, SetNeedsOneBeginFrame) {
  SurfaceId root_surface_id(
      kArbitraryFrameSinkId,
      LocalSurfaceId(1, base::UnguessableToken::Create()));

  scheduler_.SetVisible(true);
  scheduler_.SetNewRootSurface(root_surface_id);

  // Make system idle.
  AdvanceTimeAndBeginFrameForTest({root_surface_id});
  SurfaceDamaged(root_surface_id);
  scheduler_.BeginFrameDeadlineForTest();
  AdvanceTimeAndBeginFrameForTest({root_surface_id});
  scheduler_.BeginFrameDeadlineForTest();

  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());

  // SetNeedsOneBeginFrame should make DisplayScheduler active for just a single
  // BeginFrame.
  scheduler_.SetNeedsOneBeginFrame();
  EXPECT_TRUE(scheduler_.inside_begin_frame_deadline_interval());
  scheduler_.BeginFrameDeadlineForTest();
  EXPECT_EQ(BeginFrameAck(last_begin_frame_args_, false),
            client_.last_begin_frame_ack());

  // System should be idle again.
  AdvanceTimeAndBeginFrameForTest(std::vector<SurfaceId>());
  EXPECT_FALSE(scheduler_.inside_begin_frame_deadline_interval());
}

}  // namespace
}  // namespace viz

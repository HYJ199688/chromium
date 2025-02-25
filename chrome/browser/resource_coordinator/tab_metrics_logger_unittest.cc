// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/resource_coordinator/tab_metrics_logger.h"

#include <memory>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/macros.h"
#include "base/test/scoped_task_environment.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/engagement/site_engagement_service.h"
#include "chrome/browser/resource_coordinator/tab_metrics_event.pb.h"
#include "chrome/browser/resource_coordinator/tab_ranker/tab_features.h"
#include "chrome/browser/resource_coordinator/tab_ranker/tab_features_test_helper.h"
#include "chrome/browser/resource_coordinator/tab_ranker/window_features.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_activity_simulator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/test_browser_window.h"
#include "chrome/test/base/testing_profile.h"
#include "components/ukm/test_ukm_recorder.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/ui_base_types.h"

using content::WebContentsTester;
using metrics::WindowMetricsEvent;
using tab_ranker::WindowFeatures;

// Sanity checks for functions in TabMetricsLogger.
// See TabActivityWatcherTest for more thorough tab usage UKM tests.
using TabMetricsLoggerTest = ChromeRenderViewHostTestHarness;

namespace {

const char* kTestUrl = "https://example.com/";

// TestBrowserWindow whose show state can be modified.
class FakeBrowserWindow : public TestBrowserWindow {
 public:
  FakeBrowserWindow() = default;
  ~FakeBrowserWindow() override = default;

  // Helper function to handle FakeBrowserWindow lifetime. Modeled after
  // CreateBrowserWithTestWindowForParams.
  static std::unique_ptr<Browser> CreateBrowserWithFakeWindowForParams(
      Browser::CreateParams* params) {
    // TestBrowserWindowOwner takes ownersip of the window and will destroy the
    // window (along with itself) automatically when the browser is closed.
    FakeBrowserWindow* window = new FakeBrowserWindow;
    new TestBrowserWindowOwner(window);

    params->window = window;
    auto browser = std::make_unique<Browser>(*params);
    window->browser_ = browser.get();
    window->Activate();
    return browser;
  }

  // TestBrowserWindow:
  void Activate() override {
    if (is_active_)
      return;
    is_active_ = true;
    // With a real view, activating would update the BrowserList.
    BrowserList::SetLastActive(browser_);
  }
  void Deactivate() override {
    if (!is_active_)
      return;
    is_active_ = false;
    // With a real view, deactivating would notify the BrowserList.
    BrowserList::NotifyBrowserNoLongerActive(browser_);
  }
  bool IsActive() const override { return is_active_; }
  bool IsMaximized() const override {
    return show_state_ == ui::SHOW_STATE_MAXIMIZED;
  }
  bool IsMinimized() const override {
    return show_state_ == ui::SHOW_STATE_MINIMIZED;
  }
  void Maximize() override {
    show_state_ = ui::SHOW_STATE_MAXIMIZED;
    Activate();
  }
  void Minimize() override {
    show_state_ = ui::SHOW_STATE_MINIMIZED;
    Deactivate();
  }
  void Restore() override {
    // This isn't true "restore" behavior.
    show_state_ = ui::SHOW_STATE_NORMAL;
    Activate();
  }

 private:
  Browser* browser_;
  bool is_active_ = false;
  ui::WindowShowState show_state_ = ui::SHOW_STATE_NORMAL;

  DISALLOW_COPY_AND_ASSIGN(FakeBrowserWindow);
};

}  // namespace

// Tests creating a flat TabFeatures structure for logging a tab and its
// TabMetrics state.
TEST_F(TabMetricsLoggerTest, GetTabFeatures) {
  TabActivitySimulator tab_activity_simulator;
  Browser::CreateParams params(profile(), true);
  std::unique_ptr<Browser> browser =
      CreateBrowserWithTestWindowForParams(&params);
  TabStripModel* tab_strip_model = browser->tab_strip_model();

  // Add a foreground tab.
  tab_activity_simulator.AddWebContentsAndNavigate(tab_strip_model,
                                                   GURL("about://blank"));
  tab_strip_model->ActivateTabAt(0);

  // Add a background tab to test.
  content::WebContents* bg_contents =
      tab_activity_simulator.AddWebContentsAndNavigate(
          tab_strip_model, GURL("http://example.com/test.html"));
  WebContentsTester::For(bg_contents)->TestSetIsLoading(false);

  {
    TabMetricsLogger::TabMetrics bg_metrics;
    bg_metrics.web_contents = bg_contents;
    bg_metrics.page_transition = ui::PAGE_TRANSITION_FORM_SUBMIT;

    base::TimeDelta inactive_duration = base::TimeDelta::FromSeconds(10);

    tab_ranker::TabFeatures bg_features = TabMetricsLogger::GetTabFeatures(
        browser.get(), bg_metrics, inactive_duration);
    EXPECT_EQ(bg_features.has_before_unload_handler, false);
    EXPECT_EQ(bg_features.has_form_entry, false);
    EXPECT_EQ(bg_features.host, "example.com");
    EXPECT_EQ(bg_features.is_pinned, false);
    EXPECT_EQ(bg_features.key_event_count, 0);
    EXPECT_EQ(bg_features.mouse_event_count, 0);
    EXPECT_EQ(bg_features.navigation_entry_count, 1);
    EXPECT_EQ(bg_features.num_reactivations, 0);
    ASSERT_TRUE(bg_features.page_transition_core_type.has_value());
    EXPECT_EQ(bg_features.page_transition_core_type.value(), 7);
    EXPECT_EQ(bg_features.page_transition_from_address_bar, false);
    EXPECT_EQ(bg_features.page_transition_is_redirect, false);
    ASSERT_TRUE(bg_features.site_engagement_score.has_value());
    EXPECT_EQ(bg_features.site_engagement_score.value(), 0);
    EXPECT_EQ(bg_features.time_from_backgrounded,
              inactive_duration.InMilliseconds());
    EXPECT_EQ(bg_features.touch_event_count, 0);
    EXPECT_EQ(bg_features.was_recently_audible, false);
  }

  // Update tab features.
  ui::PageTransition page_transition = static_cast<ui::PageTransition>(
      ui::PAGE_TRANSITION_LINK | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  tab_activity_simulator.Navigate(bg_contents, GURL("https://www.chromium.org"),
                                  page_transition);
  tab_strip_model->SetTabPinned(1, true);
  SiteEngagementService::Get(profile())->ResetBaseScoreForURL(
      GURL("https://www.chromium.org"), 91);

  {
    TabMetricsLogger::TabMetrics bg_metrics;
    bg_metrics.web_contents = bg_contents;
    bg_metrics.page_transition = page_transition;
    bg_metrics.page_metrics.key_event_count = 3;
    bg_metrics.page_metrics.mouse_event_count = 42;
    bg_metrics.page_metrics.num_reactivations = 5;
    bg_metrics.page_metrics.touch_event_count = 10;

    base::TimeDelta inactive_duration = base::TimeDelta::FromSeconds(5);

    tab_ranker::TabFeatures bg_features = TabMetricsLogger::GetTabFeatures(
        browser.get(), bg_metrics, inactive_duration);
    EXPECT_EQ(bg_features.has_before_unload_handler, false);
    EXPECT_EQ(bg_features.has_form_entry, false);
    EXPECT_EQ(bg_features.host, "www.chromium.org");
    EXPECT_EQ(bg_features.is_pinned, true);
    EXPECT_EQ(bg_features.key_event_count, 3);
    EXPECT_EQ(bg_features.mouse_event_count, 42);
    EXPECT_EQ(bg_features.navigation_entry_count, 2);
    EXPECT_EQ(bg_features.num_reactivations, 5);
    ASSERT_TRUE(bg_features.page_transition_core_type.has_value());
    EXPECT_EQ(bg_features.page_transition_core_type.value(), 0);
    EXPECT_EQ(bg_features.page_transition_from_address_bar, true);
    EXPECT_EQ(bg_features.page_transition_is_redirect, false);
    ASSERT_TRUE(bg_features.site_engagement_score.has_value());
    // Site engagement score should round down to the nearest 10.
    EXPECT_EQ(bg_features.site_engagement_score.value(), 90);
    EXPECT_EQ(bg_features.time_from_backgrounded,
              inactive_duration.InMilliseconds());
    EXPECT_EQ(bg_features.touch_event_count, 10);
    EXPECT_EQ(bg_features.was_recently_audible, false);
  }

  tab_strip_model->CloseAllTabs();
}

// Checks that ForegroundedOrClosed event is logged correctly.
// TODO(charleszhao): add checks for TabMetrics event.
class TabMetricsLoggerUKMTest : public ::testing::Test {
 protected:
  TabMetricsLoggerUKMTest() = default;

  // Returns a new source_id associated with the test url.
  ukm::SourceId GetSourceId() {
    const ukm::SourceId source_id = ukm::UkmRecorder::GetNewSourceID();
    test_ukm_recorder_.UpdateSourceURL(source_id,
                                       GURL("https://www.chromium.org"));
    return source_id;
  }

  // Returns the fake UKM test recorder.
  ukm::TestUkmRecorder* GetTestUkmRecorder() { return &test_ukm_recorder_; }

  // Returns the TabMetricsLogger being tested.
  TabMetricsLogger* GetLogger() { return &logger_; }

  // Expects all values inside the |map| appear in the |entry|.
  void ExpectEntries(const ukm::mojom::UkmEntry* entry,
                     const base::flat_map<std::string, int64_t>& map) {
    // Check all metrics are logged as expected.
    EXPECT_EQ(entry->metrics.size(), map.size());

    for (const auto& pair : map) {
      GetTestUkmRecorder()->ExpectEntryMetric(entry, pair.first, pair.second);
    }
  }

 private:
  // Sets up the task scheduling/task-runner environment for each test.
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  // Sets itself as the global UkmRecorder on construction.
  ukm::TestAutoSetUkmRecorder test_ukm_recorder_;
  // The object being tested:
  TabMetricsLogger logger_;

  DISALLOW_COPY_AND_ASSIGN(TabMetricsLoggerUKMTest);
};

// Checks TabFeature is logged correctly with TabMetricsLogger::LogTabMetrics.
TEST_F(TabMetricsLoggerUKMTest, LogTabMetrics) {
  const tab_ranker::TabFeatures tab =
      tab_ranker::GetFullTabFeaturesForTesting();
  const int64_t query_id = 1234;
  const int64_t label_id = 5678;
  GetLogger()->set_query_id(query_id);

  GetLogger()->LogTabMetrics(GetSourceId(), tab, nullptr, label_id);

  // Checks that the size is logged correctly.
  EXPECT_EQ(1U, GetTestUkmRecorder()->sources_count());
  EXPECT_EQ(1U, GetTestUkmRecorder()->entries_count());
  const std::vector<const ukm::mojom::UkmEntry*> entries =
      GetTestUkmRecorder()->GetEntriesByName("TabManager.TabMetrics");
  EXPECT_EQ(1U, entries.size());

  // Checks that all the fields are logged correctly.
  ExpectEntries(entries[0], {
                                {"HasBeforeUnloadHandler", 1},
                                {"HasFormEntry", 1},
                                {"IsPinned", 1},
                                {"KeyEventCount", 21},
                                {"LabelId", label_id},
                                {"MouseEventCount", 22},
                                {"MRUIndex", 27},
                                {"NavigationEntryCount", 24},
                                {"NumReactivationBefore", 25},
                                {"PageTransitionCoreType", 2},
                                {"PageTransitionFromAddressBar", 1},
                                {"PageTransitionIsRedirect", 1},
                                {"QueryId", query_id},
                                {"SiteEngagementScore", 26},
                                {"TimeFromBackgrounded", 10000},
                                {"TotalTabCount", 30},
                                {"TouchEventCount", 28},
                                {"WasRecentlyAudible", 1},
                                {"WindowIsActive", 1},
                                {"WindowShowState", 3},
                                {"WindowTabCount", 27},
                                {"WindowType", 4},
                            });
}

// Checks the ForegroundedOrClosed event is logged correctly.
TEST_F(TabMetricsLoggerUKMTest, LogForegroundedOrClosedMetrics) {
  TabMetricsLogger::ForegroundedOrClosedMetrics foc_metrics;
  foc_metrics.is_foregrounded = false;
  foc_metrics.is_discarded = true;
  foc_metrics.time_from_backgrounded = 1234;
  foc_metrics.mru_index = 4;
  foc_metrics.total_tab_count = 7;
  foc_metrics.label_id = 5678;

  GetLogger()->LogForegroundedOrClosedMetrics(GetSourceId(), foc_metrics);

  // Checks that the size is logged correctly.
  EXPECT_EQ(1U, GetTestUkmRecorder()->sources_count());
  EXPECT_EQ(1U, GetTestUkmRecorder()->entries_count());
  const std::vector<const ukm::mojom::UkmEntry*> entries =
      GetTestUkmRecorder()->GetEntriesByName(
          "TabManager.Background.ForegroundedOrClosed");
  EXPECT_EQ(1U, entries.size());

  // Checks that all the fields are logged correctly.
  ExpectEntries(entries[0], {
                                {"IsDiscarded", foc_metrics.is_discarded},
                                {"IsForegrounded", foc_metrics.is_foregrounded},
                                {"LabelId", foc_metrics.label_id},
                                {"MRUIndex", foc_metrics.mru_index},
                                {"TimeFromBackgrounded",
                                 foc_metrics.time_from_backgrounded},
                                {"TotalTabCount", foc_metrics.total_tab_count},
                            });
}

// Tests WindowFeatures generated by CreateWindowFeatures.
class CreateWindowFeaturesTest : public ChromeRenderViewHostTestHarness {
 protected:
  CreateWindowFeaturesTest() = default;
  ~CreateWindowFeaturesTest() override = default;

  // Adds a tab and simulates a basic navigation.
  void AddTab(Browser* browser) {
    content::WebContentsTester::For(
        tab_activity_simulator_.AddWebContentsAndNavigate(
            browser->tab_strip_model(), GURL(kTestUrl)))
        ->TestSetIsLoading(false);
  }

 private:
  TabActivitySimulator tab_activity_simulator_;

  DISALLOW_COPY_AND_ASSIGN(CreateWindowFeaturesTest);
};

// Tests CreateWindowFeatures of two browser windows.
TEST_F(CreateWindowFeaturesTest, Basic) {
  Browser::CreateParams params(profile(), true);
  std::unique_ptr<Browser> browser =
      FakeBrowserWindow::CreateBrowserWithFakeWindowForParams(&params);

  AddTab(browser.get());
  WindowFeatures expected_metrics{WindowMetricsEvent::TYPE_TABBED,
                                  WindowMetricsEvent::SHOW_STATE_NORMAL, true,
                                  1};
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  AddTab(browser.get());
  expected_metrics.tab_count++;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  browser->window()->Minimize();
  expected_metrics.show_state = WindowMetricsEvent::SHOW_STATE_MINIMIZED;
  expected_metrics.is_active = false;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  // Create a second browser.
  Browser::CreateParams params_2(Browser::TYPE_POPUP, profile(), true);
  std::unique_ptr<Browser> browser_2 =
      FakeBrowserWindow::CreateBrowserWithFakeWindowForParams(&params_2);

  AddTab(browser_2.get());
  WindowFeatures expected_metrics_2{WindowMetricsEvent::TYPE_POPUP,
                                    WindowMetricsEvent::SHOW_STATE_NORMAL, true,
                                    1};
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser_2.get()),
            expected_metrics_2);

  // Switching the active browser.
  browser_2->window()->Deactivate();
  expected_metrics_2.is_active = false;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser_2.get()),
            expected_metrics_2);

  browser->window()->Restore();
  expected_metrics.show_state = WindowMetricsEvent::SHOW_STATE_NORMAL;
  expected_metrics.is_active = true;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  browser->tab_strip_model()->CloseAllTabs();
  browser_2->tab_strip_model()->CloseAllTabs();
}

// Tests moving a tab between browser windows.
TEST_F(CreateWindowFeaturesTest, MoveTabToOtherWindow) {
  Browser::CreateParams params(profile(), true);
  std::unique_ptr<Browser> starting_browser =
      FakeBrowserWindow::CreateBrowserWithFakeWindowForParams(&params);
  AddTab(starting_browser.get());
  WindowFeatures starting_browser_metrics{WindowMetricsEvent::TYPE_TABBED,
                                          WindowMetricsEvent::SHOW_STATE_NORMAL,
                                          true, 1};
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(starting_browser.get()),
            starting_browser_metrics);

  // Add a second tab, so we can detach it while leaving the original window
  // behind.
  AddTab(starting_browser.get());
  starting_browser_metrics.tab_count++;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(starting_browser.get()),
            starting_browser_metrics);

  // Drag the tab out of its window.
  std::unique_ptr<content::WebContents> dragged_tab =
      starting_browser->tab_strip_model()->DetachWebContentsAt(1);
  starting_browser_metrics.tab_count--;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(starting_browser.get()),
            starting_browser_metrics);

  starting_browser->window()->Deactivate();
  starting_browser_metrics.is_active = false;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(starting_browser.get()),
            starting_browser_metrics);

  // Create a new Browser for the tab.
  std::unique_ptr<Browser> created_browser =
      FakeBrowserWindow::CreateBrowserWithFakeWindowForParams(&params);
  created_browser->window()->Activate();
  created_browser->tab_strip_model()->InsertWebContentsAt(
      0, std::move(dragged_tab), TabStripModel::ADD_ACTIVE);

  WindowFeatures created_browser_metrics{WindowMetricsEvent::TYPE_TABBED,
                                         WindowMetricsEvent::SHOW_STATE_NORMAL,
                                         true, 1};
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(created_browser.get()),
            created_browser_metrics);

  starting_browser->tab_strip_model()->CloseAllTabs();
  created_browser->tab_strip_model()->CloseAllTabs();
}

// Tests replacing a tab.
TEST_F(CreateWindowFeaturesTest, ReplaceTab) {
  Browser::CreateParams params(profile(), true);
  std::unique_ptr<Browser> browser =
      FakeBrowserWindow::CreateBrowserWithFakeWindowForParams(&params);
  AddTab(browser.get());
  WindowFeatures expected_metrics{WindowMetricsEvent::TYPE_TABBED,
                                  WindowMetricsEvent::SHOW_STATE_NORMAL, true,
                                  1};
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  // Add a tab that will be replaced.
  AddTab(browser.get());
  expected_metrics.tab_count++;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  // Replace the tab.
  content::WebContents::CreateParams web_contents_params(profile(), nullptr);
  std::unique_ptr<content::WebContents> new_contents = base::WrapUnique(
      content::WebContentsTester::CreateTestWebContents(web_contents_params));
  std::unique_ptr<content::WebContents> old_contents =
      browser->tab_strip_model()->ReplaceWebContentsAt(1,
                                                       std::move(new_contents));
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  // Close the replaced tab. This should update TabCount.
  browser->tab_strip_model()->CloseWebContentsAt(1, TabStripModel::CLOSE_NONE);
  expected_metrics.tab_count--;
  EXPECT_EQ(TabMetricsLogger::CreateWindowFeatures(browser.get()),
            expected_metrics);

  browser->tab_strip_model()->CloseAllTabs();
}

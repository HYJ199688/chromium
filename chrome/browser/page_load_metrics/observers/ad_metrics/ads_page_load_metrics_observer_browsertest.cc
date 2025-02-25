// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/macros.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/metrics/subprocess_metrics_provider.h"
#include "chrome/browser/page_load_metrics/observers/ad_metrics/ads_page_load_metrics_observer.h"
#include "chrome/browser/page_load_metrics/observers/ad_metrics/frame_data.h"
#include "chrome/browser/page_load_metrics/observers/use_counter_page_load_metrics_observer.h"
#include "chrome/browser/page_load_metrics/page_load_metrics_test_waiter.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/subresource_filter/subresource_filter_browser_test_harness.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/subresource_filter/content/browser/ruleset_service.h"
#include "components/subresource_filter/core/browser/subresource_filter_features.h"
#include "components/subresource_filter/core/common/activation_scope.h"
#include "components/subresource_filter/core/common/common_features.h"
#include "components/subresource_filter/core/common/test_ruleset_utils.h"
#include "components/subresource_filter/core/mojom/subresource_filter.mojom.h"
#include "components/ukm/test_ukm_recorder.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/controllable_http_response.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "services/metrics/public/cpp/ukm_builders.h"
#include "services/metrics/public/cpp/ukm_source.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace {

const char kCrossOriginHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "OriginStatus";

const char kAdUserActivationHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "UserActivation";

const char kSqrtNumberOfPixelsHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "SqrtNumberOfPixels";

const char kSmallestDimensionHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "SmallestDimension";

const char kAdFrameSizeInterventionHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "SizeIntervention";

const char kAdFrameSizeInterventionMediaStatusHistogramId[] =
    "PageLoad.Clients.Ads.FrameCounts.AdFrames.PerFrame."
    "SizeIntervention.MediaStatus";

}  // namespace

class AdsPageLoadMetricsObserverBrowserTest
    : public subresource_filter::SubresourceFilterBrowserTest {
 public:
  AdsPageLoadMetricsObserverBrowserTest()
      : subresource_filter::SubresourceFilterBrowserTest() {
    scoped_feature_list_.InitAndEnableFeature(subresource_filter::kAdTagging);
  }
  ~AdsPageLoadMetricsObserverBrowserTest() override {}

  std::unique_ptr<page_load_metrics::PageLoadMetricsTestWaiter>
  CreatePageLoadMetricsTestWaiter() {
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    return std::make_unique<page_load_metrics::PageLoadMetricsTestWaiter>(
        web_contents);
  }

  void SetUpOnMainThread() override {
    SubresourceFilterBrowserTest::SetUpOnMainThread();
    SetRulesetWithRules(
        {subresource_filter::testing::CreateSuffixRule("ad_iframe_writer.js"),
         subresource_filter::testing::CreateSuffixRule("ad_script.js")});
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(AdsPageLoadMetricsObserverBrowserTest);
};

// Test that an embedded ad is same origin.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       OriginStatusMetricEmbedded) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("/ads_observer/srcdoc_embedded_ad.html"));
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectUniqueSample(kCrossOriginHistogramId,
                                      FrameData::OriginStatus::kSame, 1);
}

// Test that an empty embedded ad isn't reported at all.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       OriginStatusMetricEmbeddedEmpty) {
  base::HistogramTester histogram_tester;
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/ads_observer/srcdoc_embedded_ad_empty.html"));
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectTotalCount(kCrossOriginHistogramId, 0);
}

// Test that an ad with the same origin as the main page is same origin.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       OriginStatusMetricSame) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("/ads_observer/same_origin_ad.html"));
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();

  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectUniqueSample(kCrossOriginHistogramId,
                                      FrameData::OriginStatus::kSame, 1);
}

// Test that an ad with a different origin as the main page is cross origin.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       OriginStatusMetricCross) {
  // Note: Cannot navigate cross-origin without dynamically generating the URL.
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();

  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/iframe_blank.html"));
  // Note that the initial iframe is not an ad, so the metric doesn't observe
  // it initially as same origin.  However, on re-navigating to a cross
  // origin site that has an ad at its origin, the ad on that page is cross
  // origin from the original page.
  NavigateIframeToURL(web_contents(), "test",
                      embedded_test_server()->GetURL(
                          "a.com", "/ads_observer/same_origin_ad.html"));

  // Wait until all resource data updates are sent.
  waiter->AddPageExpectation(
      page_load_metrics::PageLoadMetricsTestWaiter::TimingField::kLoadEvent);
  waiter->Wait();
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectUniqueSample(kCrossOriginHistogramId,
                                      FrameData::OriginStatus::kCross, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       UserActivationSetOnFrame) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "foo.com", "/ad_tagging/frame_factory.html"));
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Create a second frame that will not receive activation.
  EXPECT_TRUE(content::ExecuteScriptWithoutUserGesture(
      web_contents, "createAdFrame('/ad_tagging/frame_factory.html', '');"));
  EXPECT_TRUE(content::ExecuteScriptWithoutUserGesture(
      web_contents, "createAdFrame('/ad_tagging/frame_factory.html', '');"));

  // Wait for the frames resources to be loaded as we only log histograms for
  // frames that have non-zero bytes. Four resources per frame and one favicon.
  waiter->AddMinimumCompleteResourcesExpectation(13);
  waiter->Wait();

  // Activate one frame by executing a dummy script.
  content::RenderFrameHost* ad_frame =
      ChildFrameAt(web_contents->GetMainFrame(), 0);
  const std::string no_op_script = "// No-op script";
  EXPECT_TRUE(ExecuteScript(ad_frame, no_op_script));

  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectBucketCount(
      kAdUserActivationHistogramId,
      FrameData::UserActivationStatus::kReceivedActivation, 1);
  histogram_tester.ExpectBucketCount(
      kAdUserActivationHistogramId,
      FrameData::UserActivationStatus::kNoActivation, 1);
}

// Test that a subframe that aborts (due to doc.write) doesn't cause a crash
// if it continues to load resources.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       DocOverwritesNavigation) {
  content::DOMMessageQueue msg_queue;

  base::HistogramTester histogram_tester;

  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/ads_observer/docwrite_provisional_frame.html"));
  std::string status;
  EXPECT_TRUE(msg_queue.WaitForMessage(&status));
  EXPECT_EQ("\"loaded\"", status);

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  // TODO(johnidel): Check that the subresources of the new frame are reported
  // correctly. Resources from a failed provisional load are not reported to
  // resource data updates, causing this adframe to not be recorded. This is an
  // uncommon case but should be reported. See crbug.com/914893.
}

// Test that a blank ad subframe that is docwritten correctly reports metrics.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       DocWriteAboutBlankAdframe) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(
                                   "/ads_observer/docwrite_blank_frame.html"));
  waiter->AddMinimumCompleteResourcesExpectation(5);
  waiter->Wait();
  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectUniqueSample(
      "PageLoad.Clients.Ads.FrameCounts.AnyParentFrame."
      "AdFrames",
      1, 1);
  histogram_tester.ExpectUniqueSample(
      "PageLoad.Clients.Ads.Bytes.AdFrames.Aggregate.Total", 0 /* < 1 KB */, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       SubresourceFilter) {
  base::HistogramTester histogram_tester;

  // cross_site_iframe_factory loads URLs like:
  // http://b.com:40919/cross_site_iframe_factory.html?b()
  SetRulesetToDisallowURLsWithPathSuffix("b()");
  const GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,b,c,d)"));

  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(browser(), main_url);

  // One favicon resource and 2 resources for each frame.
  waiter->AddMinimumCompleteResourcesExpectation(11);
  waiter->Wait();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  histogram_tester.ExpectUniqueSample(
      "PageLoad.Clients.Ads.FrameCounts.AnyParentFrame."
      "AdFrames",
      2, 1);
}

// Test that a frame without display:none is reported as visible.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       VisibleAdframeRecorded) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(
                                   "/ads_observer/display_block_adframe.html"));
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.Visible.Bytes.AdFrames.PerFrame.Total", 1);
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.Total", 1);
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.NonVisible.Bytes.AdFrames.PerFrame.Total", 0);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       DisplayNoneAdframeRecorded) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(
                                   "/ads_observer/display_none_adframe.html"));
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();
  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.NonVisible.Bytes.AdFrames.PerFrame.Total", 1);
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.Total", 1);
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.Visible.Bytes.AdFrames.PerFrame.Total", 0);
}

// TODO(https://crbug.com/929136): Investigate why setting display: none on the
// frame will cause size updates to not be received. Verify that we record the
// correct sizes for display: none iframes.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest, FramePixelSize) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/ads_observer/blank_with_adiframe_writer.html"));
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Create a 100x100 iframe.
  ASSERT_TRUE(ExecJs(
      web_contents,
      "let frame = createAdIframe(); frame.width=100; frame.height = 100; "
      "frame.src = '/ads_observer/pixel.png';"));

  // Create a 0x0 iframe.
  ASSERT_TRUE(ExecJs(
      web_contents,
      "frame = createAdIframe(); frame.width=0; frame.height = 0; frame.src = "
      "'/ads_observer/pixel.png';"));

  // Create a 10 x 1000 iframe.
  ASSERT_TRUE(
      ExecJs(web_contents,
             "frame = createAdIframe(); frame.width=10; frame.height = 1000; "
             "frame.src = '/ads_observer/pixel.png';"));

  // Wait for each frames resource to load so that they will have non-zero
  // bytes.
  waiter->AddMinimumCompleteResourcesExpectation(6);
  waiter->Wait();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectBucketCount(kSqrtNumberOfPixelsHistogramId, 100, 2);
  histogram_tester.ExpectBucketCount(kSqrtNumberOfPixelsHistogramId, 0, 1);
  histogram_tester.ExpectBucketCount(kSmallestDimensionHistogramId, 0, 1);
  histogram_tester.ExpectBucketCount(kSmallestDimensionHistogramId, 10, 1);
  histogram_tester.ExpectBucketCount(kSmallestDimensionHistogramId, 100, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       FrameWithSmallAreaNotConsideredVisible) {
  base::HistogramTester histogram_tester;
  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/ads_observer/blank_with_adiframe_writer.html"));
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Create a 4x4 iframe. The threshold for visibility is an area of 25 pixels
  // or more.
  ASSERT_TRUE(
      ExecJs(web_contents,
             "let frame = createAdIframe(); frame.width=4; frame.height = 4; "
             "frame.src = '/ads_observer/pixel.png';"));

  // Wait for each frames resource to load so that they will have non-zero
  // bytes.
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  histogram_tester.ExpectTotalCount(
      "PageLoad.Clients.Ads.Visible.FrameCounts.AdFrames.PerFrame."
      "SmallestDimension",
      0);
  histogram_tester.ExpectUniqueSample(
      "PageLoad.Clients.Ads.NonVisible.FrameCounts.AdFrames.PerFrame."
      "SmallestDimension",
      4, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverBrowserTest,
                       AdFrameSameOriginByteMetrics) {
  base::HistogramTester histogram_tester;

  // cross_site_iframe_factory loads URLs like:
  // http://b.com:40919/cross_site_iframe_factory.html?b()
  SetRulesetWithRules({subresource_filter::testing::CreateSuffixRule("b()))"),
                       subresource_filter::testing::CreateSuffixRule("e())")});
  const GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b(c(),d(b())),e(e,e()))"));

  auto waiter = CreatePageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(browser(), main_url);

  // One favicon resource and 2 resources for each frame.
  waiter->AddMinimumCompleteResourcesExpectation(17);
  waiter->Wait();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  // Verify that iframe e is only same origin.
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.PercentSameOrigin", 100, 1);

  // Verify that iframe b counts subframes as cross origin and a nested same
  // origin subframe as same origin.
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.PercentSameOrigin", 50, 1);

  // Verify that all iframe are treated as cross-origin to the page. Only 1/8 of
  // resources are on origin a.com.
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.FullPage.PercentSameOrigin", 12.5, 1);
}

class AdsPageLoadMetricsTestWaiter
    : public page_load_metrics::PageLoadMetricsTestWaiter {
 public:
  explicit AdsPageLoadMetricsTestWaiter(content::WebContents* web_contents)
      : page_load_metrics::PageLoadMetricsTestWaiter(web_contents) {}
  void AddMinimumAdResourceExpectation(int num_ad_resources) {
    expected_minimum_ad_resources_ = num_ad_resources;
  }

 protected:
  bool ExpectationsSatisfied() const override {
    return complete_ad_resources_ >= expected_minimum_ad_resources_ &&
           PageLoadMetricsTestWaiter::ExpectationsSatisfied();
  }

  void HandleResourceUpdate(
      const page_load_metrics::mojom::ResourceDataUpdatePtr& resource)
      override {
    if (resource->reported_as_ad_resource && resource->is_complete)
      complete_ad_resources_++;
  }

 private:
  int complete_ad_resources_ = 0;
  int expected_minimum_ad_resources_ = 0;
};

class AdsPageLoadMetricsObserverResourceBrowserTest
    : public subresource_filter::SubresourceFilterBrowserTest {
 public:
  AdsPageLoadMetricsObserverResourceBrowserTest() {
    scoped_feature_list_.InitAndEnableFeature(subresource_filter::kAdTagging);
  }

  ~AdsPageLoadMetricsObserverResourceBrowserTest() override {}
  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    SetRulesetWithRules(
        {subresource_filter::testing::CreateSuffixRule("ad_script.js"),
         subresource_filter::testing::CreateSuffixRule("ad_script_2.js")});
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchASCII(
        switches::kAutoplayPolicy,
        switches::autoplay::kNoUserGestureRequiredPolicy);
  }

 protected:
  std::unique_ptr<AdsPageLoadMetricsTestWaiter>
  CreateAdsPageLoadMetricsTestWaiter() {
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    return std::make_unique<AdsPageLoadMetricsTestWaiter>(web_contents);
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       ReceivedAdResources) {
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ad_tagging");
  content::SetupCrossSiteRedirector(embedded_test_server());
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("foo.com", "/frame_factory.html"));
  // Two subresources should have been reported as ads.
  waiter->AddMinimumAdResourceExpectation(2);
  waiter->Wait();
}

// Main resources for adframes are counted as ad resources.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       ReceivedMainResourceAds) {
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ad_tagging");
  content::SetupCrossSiteRedirector(embedded_test_server());
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("foo.com", "/frame_factory.html"));
  contents->GetMainFrame()->ExecuteJavaScriptForTests(
      base::ASCIIToUTF16("createAdFrame('frame_factory.html', '');"),
      base::NullCallback());
  // Two pages subresources should have been reported as ad. The iframe resource
  // and its three subresources should also be reported as ads.
  waiter->AddMinimumAdResourceExpectation(6);
  waiter->Wait();
}

// Subframe navigations report ad resources correctly.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       ReceivedSubframeNavigationAds) {
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ad_tagging");
  content::SetupCrossSiteRedirector(embedded_test_server());
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("foo.com", "/frame_factory.html"));
  contents->GetMainFrame()->ExecuteJavaScriptForTests(
      base::ASCIIToUTF16("createAdFrame('frame_factory.html', 'test');"),
      base::NullCallback());
  waiter->AddMinimumAdResourceExpectation(6);
  waiter->Wait();
  NavigateIframeToURL(
      web_contents(), "test",
      embedded_test_server()->GetURL("foo.com", "/frame_factory.html"));
  // The new subframe and its three subresources should be reported
  // as ads.
  waiter->AddMinimumAdResourceExpectation(10);
  waiter->Wait();
}

// Verify that per-resource metrics are recorded correctly.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       ReceivedAdResourceMetrics) {
  base::HistogramTester histogram_tester;

  const char kHttpResponseHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n";
  auto main_html_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/mock_page.html",
          true /*relative_url_is_prefix*/);
  auto ad_script_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/ad_script.js",
          true /*relative_url_is_prefix*/);
  auto iframe_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/iframe.html",
          true /*relative_url_is_prefix*/);
  auto vanilla_script_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/vanilla_script.js",
          true /*relative_url_is_prefix*/);
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  browser()->OpenURL(content::OpenURLParams(
      embedded_test_server()->GetURL("/mock_page.html"), content::Referrer(),
      WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));

  main_html_response->WaitForRequest();
  main_html_response->Send(kHttpResponseHeader);
  main_html_response->Send(
      "<html><body></body><script src=\"ad_script.js\"></script></html>");
  main_html_response->Send(std::string(1024, ' '));
  main_html_response->Done();

  ad_script_response->WaitForRequest();
  ad_script_response->Send(kHttpResponseHeader);
  ad_script_response->Send(
      "var iframe = document.createElement(\"iframe\");"
      "iframe.src =\"iframe.html\";"
      "document.body.appendChild(iframe);");
  ad_script_response->Send(std::string(1000, ' '));
  ad_script_response->Done();

  iframe_response->WaitForRequest();
  iframe_response->Send(kHttpResponseHeader);
  iframe_response->Send("<html><script src=\"vanilla_script.js\"></script>");
  iframe_response->Send(std::string(2000, ' '));
  iframe_response->Send("</html>");
  iframe_response->Done();

  vanilla_script_response->WaitForRequest();
  vanilla_script_response->Send(kHttpResponseHeader);
  vanilla_script_response->Send(std::string(1024, ' '));
  waiter->AddMinimumNetworkBytesExpectation(5000);
  waiter->Wait();

  // Close all tabs instead of navigating as the embedded_test_server will
  // hang waiting for loads to finish when we have an unfinished
  // ControllableHttpResponse.
  browser()->tab_strip_model()->CloseAllTabs();

  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.FullPage.Network", 5, 1);
  // We have received 4 KB of ads and 1 KB of mainframe ads.
  histogram_tester.ExpectBucketCount("PageLoad.Clients.Ads.Resources.Bytes.Ads",
                                     4, 1);
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.MainFrame.Ads.Total", 1, 1);

  // The main frame should have 2 KB of resources, 1KB from the main resource
  // and one from the ad script in the main frame.
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.MainFrame.Total", 2, 1);

  // 4 resources loaded, one unfinished.
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Resources.Bytes.Unfinished", 1, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       IncompleteResourcesRecordedToFrameMetrics) {
  base::HistogramTester histogram_tester;
  SetRulesetWithRules(
      {subresource_filter::testing::CreateSuffixRule("ad_iframe_writer.js")});
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ads_observer");
  content::SetupCrossSiteRedirector(embedded_test_server());

  const char kHttpResponseHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n";
  auto incomplete_resource_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/incomplete_resource.js",
          true /*relative_url_is_prefix*/);
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  browser()->OpenURL(content::OpenURLParams(
      embedded_test_server()->GetURL("/ad_with_incomplete_resource.html"),
      content::Referrer(), WindowOpenDisposition::CURRENT_TAB,
      ui::PAGE_TRANSITION_TYPED, false));

  waiter->AddMinimumCompleteResourcesExpectation(3);
  waiter->Wait();
  int64_t initial_page_bytes = waiter->current_network_bytes();

  // Ad resource will not finish loading but should be reported to metrics.
  incomplete_resource_response->WaitForRequest();
  incomplete_resource_response->Send(kHttpResponseHeader);
  incomplete_resource_response->Send(std::string(2048, ' '));

  // Wait for the resource update to be received for the incomplete response.
  waiter->AddMinimumNetworkBytesExpectation(2048);
  waiter->Wait();

  // Close all tabs instead of navigating as the embedded_test_server will
  // hang waiting for loads to finish when we have an unfinished
  // ControllableHttpResponse.
  browser()->tab_strip_model()->CloseAllTabs();

  int expected_page_kilobytes = (initial_page_bytes + 2048) / 1024;

  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.FullPage.Network", expected_page_kilobytes,
      1);
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.Aggregate.Network", 2, 1);
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.Aggregate.Total", 2, 1);
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.Network", 2, 1);
  histogram_tester.ExpectBucketCount(
      "PageLoad.Clients.Ads.Bytes.AdFrames.PerFrame.Total", 2, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       AdFrameSizeInterventionTriggered) {
  base::HistogramTester histogram_tester;
  SetRulesetWithRules(
      {subresource_filter::testing::CreateSuffixRule("ad_iframe_writer.js")});
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ads_observer");
  content::SetupCrossSiteRedirector(embedded_test_server());

  const char kHttpResponseHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n";
  auto resource_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/incomplete_resource.js",
          true /*relative_url_is_prefix*/);
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  browser()->OpenURL(content::OpenURLParams(
      embedded_test_server()->GetURL("/ad_with_incomplete_resource.html"),
      content::Referrer(), WindowOpenDisposition::CURRENT_TAB,
      ui::PAGE_TRANSITION_TYPED, false));

  waiter->AddMinimumCompleteResourcesExpectation(3);
  waiter->Wait();

  // Load a resource large enough to trigger intervention.
  resource_response->WaitForRequest();
  resource_response->Send(kHttpResponseHeader);
  resource_response->Send(
      std::string(FrameData::kFrameSizeInterventionByteThreshold, ' '));
  resource_response->Done();

  // Wait for the resource to finish loading.
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();

  // Close all tabs to report metrics.
  browser()->tab_strip_model()->CloseAllTabs();

  histogram_tester.ExpectBucketCount(
      kAdFrameSizeInterventionHistogramId,
      FrameData::FrameSizeInterventionStatus::kTriggered, 1);
  histogram_tester.ExpectBucketCount(
      kAdFrameSizeInterventionMediaStatusHistogramId,
      FrameData::MediaStatus::kNotPlayed, 1);
  histogram_tester.ExpectBucketCount(
      "Blink.UseCounter.Features",
      blink::mojom::WebFeature::kAdFrameSizeIntervention, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       AdFrameSizeInterventionMediaStatusPlayed) {
  base::HistogramTester histogram_tester;
  ukm::TestAutoSetUkmRecorder ukm_recorder;
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ad_tagging");
  content::SetupCrossSiteRedirector(embedded_test_server());

  const char kHttpResponseHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n";
  auto resource_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/style.css",
          true /*relative_url_is_prefix*/);
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  GURL url = embedded_test_server()->GetURL("foo.com", "/frame_factory.html");
  ui_test_utils::NavigateToURL(browser(), url);

  // This frame will load a video.
  EXPECT_TRUE(
      ExecJs(contents, "createAdFrame('multiple_mimes.html', 'test');"));

  // Intercept one of the resources loaded by "multiple_mimes.html" and load
  // enough bytes to trigger the intervention.
  resource_response->WaitForRequest();
  resource_response->Send(kHttpResponseHeader);
  resource_response->Send(
      std::string(FrameData::kFrameSizeInterventionByteThreshold, ' '));
  resource_response->Done();

  waiter->AddMinimumAdResourceExpectation(8);
  waiter->Wait();

  // Wait for the video to autoplay in the frame.
  content::RenderFrameHost* ad_frame =
      ChildFrameAt(contents->GetMainFrame(), 0);
  EXPECT_EQ("true", content::EvalJsWithManualReply(
                        ad_frame,
                        "var video = document.getElementsByTagName('video')[0];"
                        "video.onplaying = () => { "
                        "window.domAutomationController.send('true'); };"
                        "video.play();",
                        content::EXECUTE_SCRIPT_NO_USER_GESTURE));

  // Close all tabs to report metrics.
  browser()->tab_strip_model()->CloseAllTabs();

  histogram_tester.ExpectBucketCount(
      kAdFrameSizeInterventionHistogramId,
      FrameData::FrameSizeInterventionStatus::kTriggered, 1);
  histogram_tester.ExpectBucketCount(
      kAdFrameSizeInterventionMediaStatusHistogramId,
      FrameData::MediaStatus::kPlayed, 1);
  histogram_tester.ExpectBucketCount(
      "Blink.UseCounter.Features",
      blink::mojom::WebFeature::kAdFrameSizeIntervention, 1);
}

IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       AdFrameSizeInterventionNotActivatedOnFrameWithGesture) {
  base::HistogramTester histogram_tester;
  SetRulesetWithRules(
      {subresource_filter::testing::CreateSuffixRule("ad_iframe_writer.js")});
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ads_observer");
  content::SetupCrossSiteRedirector(embedded_test_server());

  const char kHttpResponseHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n";
  auto resource_response =
      std::make_unique<net::test_server::ControllableHttpResponse>(
          embedded_test_server(), "/incomplete_resource.js",
          true /*relative_url_is_prefix*/);
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  browser()->OpenURL(content::OpenURLParams(
      embedded_test_server()->GetURL("/ad_with_incomplete_resource.html"),
      content::Referrer(), WindowOpenDisposition::CURRENT_TAB,
      ui::PAGE_TRANSITION_TYPED, false));

  waiter->AddMinimumCompleteResourcesExpectation(3);
  waiter->Wait();

  // Activate one frame by executing a dummy script.
  content::RenderFrameHost* ad_frame =
      ChildFrameAt(web_contents()->GetMainFrame(), 0);
  const std::string no_op_script = "// No-op script";
  EXPECT_TRUE(ExecuteScript(ad_frame, no_op_script));

  // Load a resource large enough to trigger intervention.
  resource_response->WaitForRequest();
  resource_response->Send(kHttpResponseHeader);
  resource_response->Send(
      std::string(FrameData::kFrameSizeInterventionByteThreshold, ' '));
  resource_response->Done();

  // Wait for the resource to finish loading.
  waiter->AddMinimumCompleteResourcesExpectation(4);
  waiter->Wait();

  // Close all tabs to report metrics.
  browser()->tab_strip_model()->CloseAllTabs();

  histogram_tester.ExpectBucketCount(
      kAdFrameSizeInterventionHistogramId,
      FrameData::FrameSizeInterventionStatus::kNone, 1);
  histogram_tester.ExpectTotalCount(
      kAdFrameSizeInterventionMediaStatusHistogramId, 0);
  histogram_tester.ExpectBucketCount(
      "Blink.UseCounter.Features",
      blink::mojom::WebFeature::kAdFrameSizeIntervention, 0);
}

// Verify that UKM metrics are recorded correctly.
IN_PROC_BROWSER_TEST_F(AdsPageLoadMetricsObserverResourceBrowserTest,
                       RecordedUKMMetrics) {
  base::HistogramTester histogram_tester;
  ukm::TestAutoSetUkmRecorder ukm_recorder;
  embedded_test_server()->ServeFilesFromSourceDirectory(
      "chrome/test/data/ad_tagging");
  content::SetupCrossSiteRedirector(embedded_test_server());
  ASSERT_TRUE(embedded_test_server()->Start());

  auto waiter = CreateAdsPageLoadMetricsTestWaiter();

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  GURL url = embedded_test_server()->GetURL("foo.com", "/frame_factory.html");
  ui_test_utils::NavigateToURL(browser(), url);
  contents->GetMainFrame()->ExecuteJavaScriptForTests(
      base::ASCIIToUTF16("createAdFrame('multiple_mimes.html', 'test');"),
      base::NullCallback());
  waiter->AddMinimumAdResourceExpectation(8);
  waiter->Wait();

  // Close all tabs to report metrics.
  browser()->tab_strip_model()->CloseAllTabs();

  // Verify UKM Metrics recorded.
  auto entries =
      ukm_recorder.GetEntriesByName(ukm::builders::AdPageLoad::kEntryName);
  EXPECT_EQ(1u, entries.size());
  ukm_recorder.ExpectEntrySourceHasUrl(entries.front(), url);
  EXPECT_GT(*ukm_recorder.GetEntryMetric(
                entries.front(), ukm::builders::AdPageLoad::kAdBytesName),
            0);
  EXPECT_GT(
      *ukm_recorder.GetEntryMetric(
          entries.front(), ukm::builders::AdPageLoad::kAdBytesPerSecondName),
      0);

  // TTI is not reached by this page and thus should not have this recorded.
  EXPECT_FALSE(ukm_recorder.EntryHasMetric(
      entries.front(),
      ukm::builders::AdPageLoad::kAdBytesPerSecondAfterInteractiveName));
  EXPECT_GT(
      *ukm_recorder.GetEntryMetric(
          entries.front(), ukm::builders::AdPageLoad::kAdJavascriptBytesName),
      0);
  EXPECT_GT(*ukm_recorder.GetEntryMetric(
                entries.front(), ukm::builders::AdPageLoad::kAdVideoBytesName),
            0);
}

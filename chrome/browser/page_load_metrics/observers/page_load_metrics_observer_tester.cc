// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/page_load_metrics/observers/page_load_metrics_observer_tester.h"

#include <memory>
#include <string>
#include <utility>

#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/page_load_metrics/metrics_web_contents_observer.h"
#include "chrome/browser/page_load_metrics/page_load_metrics_embedder_interface.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/resource_type.h"
#include "net/base/ip_endpoint.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_input_event.h"
#include "url/gurl.h"

namespace page_load_metrics {

namespace {

class TestPageLoadMetricsEmbedderInterface
    : public PageLoadMetricsEmbedderInterface {
 public:
  explicit TestPageLoadMetricsEmbedderInterface(
      PageLoadMetricsObserverTester* test)
      : test_(test) {}

  bool IsNewTabPageUrl(const GURL& url) override { return false; }

  // Forward the registration logic to the test class so that derived classes
  // can override the logic there without depending on the embedder interface.
  void RegisterObservers(PageLoadTracker* tracker) override {
    test_->RegisterObservers(tracker);
  }

  std::unique_ptr<base::OneShotTimer> CreateTimer() override {
    auto timer = std::make_unique<test::WeakMockTimer>();
    test_->SetMockTimer(timer->AsWeakPtr());
    return std::move(timer);
  }

 private:
  PageLoadMetricsObserverTester* test_;

  DISALLOW_COPY_AND_ASSIGN(TestPageLoadMetricsEmbedderInterface);
};

}  // namespace

PageLoadMetricsObserverTester::PageLoadMetricsObserverTester(
    content::WebContents* web_contents,
    const RegisterObserversCallback& callback)
    : register_callback_(callback),
      web_contents_(web_contents),
      observer_(MetricsWebContentsObserver::CreateForWebContents(
          web_contents,
          std::make_unique<TestPageLoadMetricsEmbedderInterface>(this))) {}

PageLoadMetricsObserverTester::~PageLoadMetricsObserverTester() {}

void PageLoadMetricsObserverTester::SimulateTimingUpdate(
    const mojom::PageLoadTiming& timing) {
  SimulateTimingUpdate(timing, web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateTimingUpdate(
    const mojom::PageLoadTiming& timing,
    content::RenderFrameHost* rfh) {
  SimulatePageLoadTimingUpdate(
      timing, mojom::PageLoadMetadata(), mojom::PageLoadFeatures(),
      mojom::FrameRenderDataUpdate(), mojom::CpuTiming(), rfh);
}

void PageLoadMetricsObserverTester::SimulateCpuTimingUpdate(
    const mojom::CpuTiming& cpu_timing) {
  SimulateCpuTimingUpdate(cpu_timing, web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateCpuTimingUpdate(
    const mojom::CpuTiming& cpu_timing,
    content::RenderFrameHost* rfh) {
  auto timing = page_load_metrics::mojom::PageLoadTimingPtr(base::in_place);
  page_load_metrics::InitPageLoadTimingForTest(timing.get());
  SimulatePageLoadTimingUpdate(*timing, mojom::PageLoadMetadata(),
                               mojom::PageLoadFeatures(),
                               mojom::FrameRenderDataUpdate(), cpu_timing, rfh);
}

void PageLoadMetricsObserverTester::SimulateTimingAndMetadataUpdate(
    const mojom::PageLoadTiming& timing,
    const mojom::PageLoadMetadata& metadata) {
  SimulatePageLoadTimingUpdate(timing, metadata, mojom::PageLoadFeatures(),
                               mojom::FrameRenderDataUpdate(),
                               mojom::CpuTiming(),
                               web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateMetadataUpdate(
    const mojom::PageLoadMetadata& metadata,
    content::RenderFrameHost* rfh) {
  mojom::PageLoadTiming timing;
  InitPageLoadTimingForTest(&timing);
  SimulatePageLoadTimingUpdate(timing, metadata, mojom::PageLoadFeatures(),
                               mojom::FrameRenderDataUpdate(),
                               mojom::CpuTiming(), rfh);
}

void PageLoadMetricsObserverTester::SimulateFeaturesUpdate(
    const mojom::PageLoadFeatures& new_features) {
  SimulatePageLoadTimingUpdate(
      mojom::PageLoadTiming(), mojom::PageLoadMetadata(), new_features,
      mojom::FrameRenderDataUpdate(), mojom::CpuTiming(),
      web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateRenderDataUpdate(
    const mojom::FrameRenderDataUpdate& render_data) {
  SimulateRenderDataUpdate(render_data, web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateRenderDataUpdate(
    const mojom::FrameRenderDataUpdate& render_data,
    content::RenderFrameHost* rfh) {
  mojom::PageLoadTiming timing;
  InitPageLoadTimingForTest(&timing);
  SimulatePageLoadTimingUpdate(timing, mojom::PageLoadMetadata(),
                               mojom::PageLoadFeatures(), render_data,
                               mojom::CpuTiming(), rfh);
}

void PageLoadMetricsObserverTester::SimulatePageLoadTimingUpdate(
    const mojom::PageLoadTiming& timing,
    const mojom::PageLoadMetadata& metadata,
    const mojom::PageLoadFeatures& new_features,
    const mojom::FrameRenderDataUpdate& render_data,
    const mojom::CpuTiming& cpu_timing,
    content::RenderFrameHost* rfh) {
  observer_->OnTimingUpdated(rfh, timing.Clone(), metadata.Clone(),
                             new_features.Clone(),
                             std::vector<mojom::ResourceDataUpdatePtr>(),
                             render_data.Clone(), cpu_timing.Clone());
  // If sending the timing update caused the PageLoadMetricsUpdateDispatcher to
  // schedule a buffering timer, then fire it now so metrics are dispatched to
  // observers.
  base::MockOneShotTimer* mock_timer = GetMockTimer();
  if (mock_timer && mock_timer->IsRunning())
    mock_timer->Fire();
}

void PageLoadMetricsObserverTester::SimulateResourceDataUseUpdate(
    const std::vector<mojom::ResourceDataUpdatePtr>& resources) {
  SimulateResourceDataUseUpdate(resources, web_contents()->GetMainFrame());
}

void PageLoadMetricsObserverTester::SimulateResourceDataUseUpdate(
    const std::vector<mojom::ResourceDataUpdatePtr>& resources,
    content::RenderFrameHost* render_frame_host) {
  auto timing = mojom::PageLoadTimingPtr(base::in_place);
  InitPageLoadTimingForTest(timing.get());
  observer_->OnTimingUpdated(render_frame_host, std::move(timing),
                             mojom::PageLoadMetadataPtr(base::in_place),
                             mojom::PageLoadFeaturesPtr(base::in_place),
                             resources,
                             mojom::FrameRenderDataUpdatePtr(base::in_place),
                             mojom::CpuTimingPtr(base::in_place));
}

void PageLoadMetricsObserverTester::SimulateLoadedResource(
    const ExtraRequestCompleteInfo& info) {
  SimulateLoadedResource(info, content::GlobalRequestID());
}

void PageLoadMetricsObserverTester::SimulateLoadedResource(
    const ExtraRequestCompleteInfo& info,
    const content::GlobalRequestID& request_id) {
  if (info.resource_type == content::RESOURCE_TYPE_MAIN_FRAME) {
    ASSERT_NE(content::GlobalRequestID(), request_id)
        << "Main frame resources must have a GlobalRequestID.";
  }

  // For consistency with browser-side navigation, we provide a null RFH for
  // main frame and sub frame resources.
  content::RenderFrameHost* render_frame_host_or_null =
      (info.resource_type == content::RESOURCE_TYPE_MAIN_FRAME ||
       info.resource_type == content::RESOURCE_TYPE_SUB_FRAME)
          ? nullptr
          : web_contents()->GetMainFrame();

  observer_->OnRequestComplete(
      info.url, info.remote_endpoint, info.frame_tree_node_id, request_id,
      render_frame_host_or_null, info.resource_type, info.was_cached,
      info.data_reduction_proxy_data
          ? info.data_reduction_proxy_data->DeepCopy()
          : nullptr,
      info.raw_body_bytes, info.original_network_content_length,
      base::TimeTicks::Now(), info.net_error,
      info.load_timing_info
          ? std::make_unique<net::LoadTimingInfo>(*info.load_timing_info)
          : nullptr);
}

void PageLoadMetricsObserverTester::SimulateFrameReceivedFirstUserActivation(
    content::RenderFrameHost* render_frame_host) {
  observer_->FrameReceivedFirstUserActivation(render_frame_host);
}

void PageLoadMetricsObserverTester::SimulateInputEvent(
    const blink::WebInputEvent& event) {
  observer_->OnInputEvent(event);
}

void PageLoadMetricsObserverTester::SimulateAppEnterBackground() {
  observer_->FlushMetricsOnAppEnterBackground();
}

void PageLoadMetricsObserverTester::SimulateMediaPlayed() {
  content::WebContentsObserver::MediaPlayerInfo video_type(
      true /* has_video*/, true /* has_audio */);
  content::RenderFrameHost* render_frame_host = web_contents()->GetMainFrame();
  observer_->MediaStartedPlaying(
      video_type,
      content::WebContentsObserver::MediaPlayerId(render_frame_host, 0));
}

MetricsWebContentsObserver* PageLoadMetricsObserverTester::observer() const {
  return observer_;
}

const PageLoadExtraInfo
PageLoadMetricsObserverTester::GetPageLoadExtraInfoForCommittedLoad() {
  return observer_->GetPageLoadExtraInfoForCommittedLoad();
}

void PageLoadMetricsObserverTester::RegisterObservers(
    PageLoadTracker* tracker) {
  if (!register_callback_.is_null())
    register_callback_.Run(tracker);
}

}  // namespace page_load_metrics

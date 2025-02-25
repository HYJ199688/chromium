// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/page_load_metrics/observers/data_saver_site_breakdown_metrics_observer.h"

#include "chrome/browser/data_reduction_proxy/data_reduction_proxy_chrome_settings.h"
#include "chrome/browser/data_reduction_proxy/data_reduction_proxy_chrome_settings_factory.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_service.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_settings.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_params.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/features.h"
#include "url/gurl.h"

DataSaverSiteBreakdownMetricsObserver::DataSaverSiteBreakdownMetricsObserver() =
    default;

DataSaverSiteBreakdownMetricsObserver::
    ~DataSaverSiteBreakdownMetricsObserver() = default;

page_load_metrics::PageLoadMetricsObserver::ObservePolicy
DataSaverSiteBreakdownMetricsObserver::OnCommit(
    content::NavigationHandle* navigation_handle,
    ukm::SourceId source_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!data_reduction_proxy::params::IsDataSaverSiteBreakdownUsingPLMEnabled())
    return STOP_OBSERVING;

  // This BrowserContext is valid for the lifetime of
  // DataReductionProxyMetricsObserver. BrowserContext is always valid and
  // non-nullptr in NavigationControllerImpl, which is a member of WebContents.
  // A raw pointer to BrowserContext taken at this point will be valid until
  // after WebContent's destructor. The latest that PageLoadTracker's destructor
  // will be called is in MetricsWebContentsObserver's destructor, which is
  // called in WebContents destructor.
  browser_context_ = navigation_handle->GetWebContents()->GetBrowserContext();

  // Use Virtual URL instead of actual host.
  committed_host_ = navigation_handle->GetWebContents()
                        ->GetLastCommittedURL()
                        .HostNoBrackets();
  return CONTINUE_OBSERVING;
}

void DataSaverSiteBreakdownMetricsObserver::OnResourceDataUseObserved(
    content::RenderFrameHost* rfh,
    const std::vector<page_load_metrics::mojom::ResourceDataUpdatePtr>&
        resources) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  data_reduction_proxy::DataReductionProxySettings*
      data_reduction_proxy_settings =
          DataReductionProxyChromeSettingsFactory::GetForBrowserContext(
              browser_context_);
  if (data_reduction_proxy_settings &&
      data_reduction_proxy_settings->data_reduction_proxy_service()) {
    DCHECK(!committed_host_.empty());
    int64_t received_data_length = 0;
    int64_t data_reduction_proxy_bytes_saved = 0;
    for (auto const& resource : resources) {
      received_data_length += resource->delta_bytes;

      // Estimate savings based on network bytes used.
      data_reduction_proxy_bytes_saved +=
          resource->delta_bytes *
          (resource->data_reduction_proxy_compression_ratio_estimate - 1.0);

      if (resource->is_complete) {
        // Record the actual data savings based on body length. Remove
        // previously added savings from network usage.
        data_reduction_proxy_bytes_saved +=
            (resource->encoded_body_length - resource->received_data_length) *
            (resource->data_reduction_proxy_compression_ratio_estimate - 1.0);
      }
    }
    data_reduction_proxy_settings->data_reduction_proxy_service()
        ->UpdateDataUseForHost(
            received_data_length,
            received_data_length + data_reduction_proxy_bytes_saved,
            committed_host_);
    if (base::FeatureList::IsEnabled(network::features::kNetworkService)) {
      // TODO(rajendrant): Fix the |request_type| and |mime_type| sent below or
      // remove the respective histograms.
      data_reduction_proxy_settings->data_reduction_proxy_service()
          ->UpdateContentLengths(
              received_data_length,
              received_data_length + data_reduction_proxy_bytes_saved,
              data_reduction_proxy_settings->IsDataReductionProxyEnabled(),
              data_reduction_proxy::VIA_DATA_REDUCTION_PROXY,
              std::string() /* mime_type */, true /*is_user_traffic*/,
              data_use_measurement::DataUseUserData::OTHER, 0);
    }
  }
}

page_load_metrics::PageLoadMetricsObserver::ObservePolicy
DataSaverSiteBreakdownMetricsObserver::ShouldObserveMimeType(
    const std::string& mime_type) const {
  // Observe all MIME types. We still only use actual data usage, so strange
  // cases (e.g., data:// URLs) will still record the right amount of data
  // usage.
  return CONTINUE_OBSERVING;
}

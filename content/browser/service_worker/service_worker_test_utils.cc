// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_test_utils.h"

#include <memory>
#include <utility>

#include "base/barrier_closure.h"
#include "base/run_loop.h"
#include "base/time/time.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_database.h"
#include "content/browser/service_worker/service_worker_disk_cache.h"
#include "content/browser/service_worker/service_worker_provider_host.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_storage.h"
#include "content/common/frame_messages.h"
#include "content/common/frame_messages.mojom.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/transferrable_url_loader.mojom.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "net/base/io_buffer.h"
#include "net/base/test_completion_callback.h"
#include "net/http/http_response_info.h"
#include "third_party/blink/public/common/service_worker/service_worker_utils.h"

namespace content {

namespace {

// A mock SharedURLLoaderFactory that always fails to start.
// TODO(bashi): Make this factory not to fail when unit tests actually need
// this to be working.
class MockSharedURLLoaderFactory final
    : public network::SharedURLLoaderFactory {
 public:
  MockSharedURLLoaderFactory() = default;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(network::mojom::URLLoaderRequest request,
                            int32_t routing_id,
                            int32_t request_id,
                            uint32_t options,
                            const network::ResourceRequest& url_request,
                            network::mojom::URLLoaderClientPtr client,
                            const net::MutableNetworkTrafficAnnotationTag&
                                traffic_annotation) override {
    client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_NOT_IMPLEMENTED));
  }
  void Clone(network::mojom::URLLoaderFactoryRequest request) override {
    NOTREACHED();
  }

  // network::SharedURLLoaderFactory:
  std::unique_ptr<network::SharedURLLoaderFactoryInfo> Clone() override {
    NOTREACHED();
    return nullptr;
  }

 private:
  friend class base::RefCounted<MockSharedURLLoaderFactory>;

  ~MockSharedURLLoaderFactory() override = default;

  DISALLOW_COPY_AND_ASSIGN(MockSharedURLLoaderFactory);
};

// Returns MockSharedURLLoaderFactory.
class MockSharedURLLoaderFactoryInfo final
    : public network::SharedURLLoaderFactoryInfo {
 public:
  MockSharedURLLoaderFactoryInfo() = default;
  ~MockSharedURLLoaderFactoryInfo() override = default;

 protected:
  scoped_refptr<network::SharedURLLoaderFactory> CreateFactory() override {
    return base::MakeRefCounted<MockSharedURLLoaderFactory>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MockSharedURLLoaderFactoryInfo);
};

class FakeNavigationClient : public mojom::NavigationClient {
 public:
  using ReceivedProviderInfoCallback = base::OnceCallback<void(
      blink::mojom::ServiceWorkerProviderInfoForWindowPtr)>;

  FakeNavigationClient(ReceivedProviderInfoCallback on_received_callback)
      : on_received_callback_(std::move(on_received_callback)) {}
  ~FakeNavigationClient() override = default;

 private:
  // mojom::NavigationClientPtr implementation:
  void CommitNavigation(
      const network::ResourceResponseHead& head,
      const CommonNavigationParams& common_params,
      const CommitNavigationParams& commit_params,
      network::mojom::URLLoaderClientEndpointsPtr url_loader_client_endpoints,
      std::unique_ptr<blink::URLLoaderFactoryBundleInfo> subresource_loaders,
      base::Optional<std::vector<::content::mojom::TransferrableURLLoaderPtr>>
          subresource_overrides,
      blink::mojom::ControllerServiceWorkerInfoPtr
          controller_service_worker_info,
      blink::mojom::ServiceWorkerProviderInfoForWindowPtr provider_info,
      network::mojom::URLLoaderFactoryPtr prefetch_loader_factory,
      const base::UnguessableToken& devtools_navigation_token,
      CommitNavigationCallback callback) override {
    std::move(on_received_callback_).Run(std::move(provider_info));
    std::move(callback).Run(nullptr, nullptr);
  }
  void CommitFailedNavigation(
      const CommonNavigationParams& common_params,
      const CommitNavigationParams& commit_params,
      bool has_stale_copy_in_cache,
      int error_code,
      const base::Optional<std::string>& error_page_content,
      std::unique_ptr<blink::URLLoaderFactoryBundleInfo> subresource_loaders,
      CommitFailedNavigationCallback callback) override {
    std::move(callback).Run(nullptr, nullptr);
  }

  ReceivedProviderInfoCallback on_received_callback_;

  DISALLOW_COPY_AND_ASSIGN(FakeNavigationClient);
};

void OnWriteBodyInfoToDiskCache(
    std::unique_ptr<ServiceWorkerResponseWriter> writer,
    const std::string& body,
    base::OnceClosure callback,
    int result) {
  EXPECT_GE(result, 0);
  scoped_refptr<net::IOBuffer> body_buffer =
      base::MakeRefCounted<net::StringIOBuffer>(body);
  ServiceWorkerResponseWriter* writer_rawptr = writer.get();
  writer_rawptr->WriteData(
      body_buffer.get(), body.size(),
      base::BindOnce(
          [](std::unique_ptr<ServiceWorkerResponseWriter> /* unused */,
             base::OnceClosure callback, int expected, int result) {
            EXPECT_EQ(expected, result);
            std::move(callback).Run();
          },
          std::move(writer), std::move(callback), body.size()));
}

void WriteBodyToDiskCache(std::unique_ptr<ServiceWorkerResponseWriter> writer,
                          std::unique_ptr<net::HttpResponseInfo> info,
                          const std::string& body,
                          base::OnceClosure callback) {
  scoped_refptr<HttpResponseInfoIOBuffer> info_buffer =
      base::MakeRefCounted<HttpResponseInfoIOBuffer>(std::move(info));
  info_buffer->response_data_size = body.size();
  ServiceWorkerResponseWriter* writer_rawptr = writer.get();
  writer_rawptr->WriteInfo(
      info_buffer.get(),
      base::BindOnce(&OnWriteBodyInfoToDiskCache, std::move(writer), body,
                     std::move(callback)));
}

void WriteMetaDataToDiskCache(
    std::unique_ptr<ServiceWorkerResponseMetadataWriter> writer,
    const std::string& meta_data,
    base::OnceClosure callback) {
  scoped_refptr<net::IOBuffer> meta_data_buffer =
      base::MakeRefCounted<net::StringIOBuffer>(meta_data);
  ServiceWorkerResponseMetadataWriter* writer_rawptr = writer.get();
  writer_rawptr->WriteMetadata(
      meta_data_buffer.get(), meta_data.size(),
      base::BindOnce(
          [](std::unique_ptr<ServiceWorkerResponseMetadataWriter> /* unused */,
             base::OnceClosure callback, int expected, int result) {
            EXPECT_EQ(expected, result);
            std::move(callback).Run();
          },
          std::move(writer), std::move(callback), meta_data.size()));
}

}  // namespace

ServiceWorkerRemoteProviderEndpoint::ServiceWorkerRemoteProviderEndpoint() {}
ServiceWorkerRemoteProviderEndpoint::ServiceWorkerRemoteProviderEndpoint(
    ServiceWorkerRemoteProviderEndpoint&& other)
    : navigation_client_(std::move(other.navigation_client_)),
      host_ptr_(std::move(other.host_ptr_)),
      client_request_(std::move(other.client_request_)) {}

ServiceWorkerRemoteProviderEndpoint::~ServiceWorkerRemoteProviderEndpoint() {}

void ServiceWorkerRemoteProviderEndpoint::BindForWindow(
    blink::mojom::ServiceWorkerProviderInfoForWindowPtr info) {
  // We establish a message pipe for connecting |navigation_client_| to a fake
  // navigation client, then simulate sending the navigation commit IPC which
  // carries a service worker provider info over it, then the provider info
  // received there gets its |host_ptr_info| and |client_request| associated
  // with a message pipe so that their users later can make Mojo calls without
  // crash.
  blink::mojom::ServiceWorkerProviderInfoForWindowPtr received_info;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  mojo::MakeStrongBinding(
      std::make_unique<FakeNavigationClient>(base::BindOnce(
          [](base::OnceClosure quit_closure,
             blink::mojom::ServiceWorkerProviderInfoForWindowPtr* out_info,
             blink::mojom::ServiceWorkerProviderInfoForWindowPtr info) {
            *out_info = std::move(info);
            std::move(quit_closure).Run();
          },
          loop.QuitClosure(), &received_info)),
      mojo::MakeRequest(&navigation_client_));
  navigation_client_->CommitNavigation(
      network::ResourceResponseHead(), content::CommonNavigationParams(),
      content::CommitNavigationParams(), nullptr, nullptr, base::nullopt,
      nullptr, std::move(info), nullptr, base::UnguessableToken::Create(),
      base::BindOnce(
          [](std::unique_ptr<FrameHostMsg_DidCommitProvisionalLoad_Params>
                 validated_params,
             mojom::DidCommitProvisionalLoadInterfaceParamsPtr
                 interface_params) {}));
  loop.Run();

  client_request_ = std::move(received_info->client_request);
  host_ptr_.Bind(std::move(received_info->host_ptr_info));
}

void ServiceWorkerRemoteProviderEndpoint::BindForServiceWorker(
    blink::mojom::ServiceWorkerProviderInfoForStartWorkerPtr info) {
  client_request_ = std::move(info->client_request);
  host_ptr_.Bind(std::move(info->host_ptr_info));
}

base::WeakPtr<ServiceWorkerProviderHost> CreateProviderHostForWindow(
    int process_id,
    bool is_parent_frame_secure,
    base::WeakPtr<ServiceWorkerContextCore> context,
    ServiceWorkerRemoteProviderEndpoint* output_endpoint) {
  auto provider_info = blink::mojom::ServiceWorkerProviderInfoForWindow::New();
  base::WeakPtr<ServiceWorkerProviderHost> host =
      ServiceWorkerProviderHost::PreCreateNavigationHost(
          context, is_parent_frame_secure, base::NullCallback(),
          &provider_info);
  output_endpoint->BindForWindow(std::move(provider_info));

  // In production code this is called from NavigationRequest in the browser
  // process right before navigation commit.
  host->OnBeginNavigationCommit(process_id, 1 /* route_id */);
  return host;
}

base::WeakPtr<ServiceWorkerProviderHost>
CreateProviderHostForServiceWorkerContext(
    int process_id,
    bool is_parent_frame_secure,
    ServiceWorkerVersion* hosted_version,
    base::WeakPtr<ServiceWorkerContextCore> context,
    ServiceWorkerRemoteProviderEndpoint* output_endpoint) {
  auto provider_info =
      blink::mojom::ServiceWorkerProviderInfoForStartWorker::New();
  base::WeakPtr<ServiceWorkerProviderHost> host =
      ServiceWorkerProviderHost::PreCreateForController(
          std::move(context), base::WrapRefCounted(hosted_version),
          &provider_info);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory;
  if (blink::ServiceWorkerUtils::IsServicificationEnabled()) {
    loader_factory = network::SharedURLLoaderFactory::Create(
        std::make_unique<MockSharedURLLoaderFactoryInfo>());
  }

  provider_info = host->CompleteStartWorkerPreparation(
      process_id, loader_factory, std::move(provider_info));
  output_endpoint->BindForServiceWorker(std::move(provider_info));
  return host;
}

ServiceWorkerDatabase::ResourceRecord WriteToDiskCacheSync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::string& meta_data) {
  base::RunLoop loop;
  ServiceWorkerDatabase::ResourceRecord record =
      WriteToDiskCacheAsync(storage, script_url, resource_id, headers, body,
                            meta_data, loop.QuitClosure());
  loop.Run();
  return record;
}

ServiceWorkerDatabase::ResourceRecord
WriteToDiskCacheWithCustomResponseInfoSync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    std::unique_ptr<net::HttpResponseInfo> http_info,
    const std::string& body,
    const std::string& meta_data) {
  base::RunLoop loop;
  ServiceWorkerDatabase::ResourceRecord record =
      WriteToDiskCacheWithCustomResponseInfoAsync(
          storage, script_url, resource_id, std::move(http_info), body,
          meta_data, loop.QuitClosure());
  loop.Run();
  return record;
}

ServiceWorkerDatabase::ResourceRecord WriteToDiskCacheAsync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::string& meta_data,
    base::OnceClosure callback) {
  std::unique_ptr<net::HttpResponseInfo> info =
      std::make_unique<net::HttpResponseInfo>();
  info->request_time = base::Time::Now();
  info->response_time = base::Time::Now();
  info->headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.0 200 OK\0\0");
  for (const auto& header : headers)
    info->headers->AddHeader(header.first + ": " + header.second);
  return WriteToDiskCacheWithCustomResponseInfoAsync(
      storage, script_url, resource_id, std::move(info), body, meta_data,
      std::move(callback));
}

ServiceWorkerDatabase::ResourceRecord
WriteToDiskCacheWithCustomResponseInfoAsync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    std::unique_ptr<net::HttpResponseInfo> http_info,
    const std::string& body,
    const std::string& meta_data,
    base::OnceClosure callback) {
  base::RepeatingClosure barrier = base::BarrierClosure(2, std::move(callback));
  auto body_writer = storage->CreateResponseWriter(resource_id);
  WriteBodyToDiskCache(std::move(body_writer), std::move(http_info), body,
                       barrier);
  auto metadata_writer = storage->CreateResponseMetadataWriter(resource_id);
  WriteMetaDataToDiskCache(std::move(metadata_writer), meta_data,
                           std::move(barrier));
  return ServiceWorkerDatabase::ResourceRecord(resource_id, script_url,
                                               body.size());
}

MockServiceWorkerResponseReader::MockServiceWorkerResponseReader()
    : ServiceWorkerResponseReader(/* resource_id=*/0, /*disk_cache=*/nullptr) {}

MockServiceWorkerResponseReader::~MockServiceWorkerResponseReader() {}

void MockServiceWorkerResponseReader::ReadInfo(
    HttpResponseInfoIOBuffer* info_buf,
    OnceCompletionCallback callback) {
  DCHECK(!expected_reads_.empty());
  ExpectedRead expected = expected_reads_.front();
  EXPECT_TRUE(expected.info);
  if (expected.async) {
    pending_info_ = info_buf;
    pending_callback_ = std::move(callback);
  } else {
    expected_reads_.pop();
    info_buf->response_data_size = expected.len;
    std::move(callback).Run(expected.result);
  }
}

void MockServiceWorkerResponseReader::ReadData(
    net::IOBuffer* buf,
    int buf_len,
    OnceCompletionCallback callback) {
  DCHECK(!expected_reads_.empty());
  ExpectedRead expected = expected_reads_.front();
  EXPECT_FALSE(expected.info);
  if (expected.async) {
    pending_callback_ = std::move(callback);
    pending_buffer_ = buf;
    pending_buffer_len_ = static_cast<size_t>(buf_len);
  } else {
    expected_reads_.pop();
    if (expected.len > 0) {
      size_t to_read = std::min(static_cast<size_t>(buf_len), expected.len);
      memcpy(buf->data(), expected.data, to_read);
    }
    std::move(callback).Run(expected.result);
  }
}

void MockServiceWorkerResponseReader::ExpectReadInfo(size_t len,
                                                     bool async,
                                                     int result) {
  expected_reads_.push(ExpectedRead(len, async, result));
}

void MockServiceWorkerResponseReader::ExpectReadInfoOk(size_t len, bool async) {
  expected_reads_.push(ExpectedRead(len, async, len));
}

void MockServiceWorkerResponseReader::ExpectReadData(const char* data,
                                                     size_t len,
                                                     bool async,
                                                     int result) {
  expected_reads_.push(ExpectedRead(data, len, async, result));
}

void MockServiceWorkerResponseReader::ExpectReadDataOk(const std::string& data,
                                                       bool async) {
  expected_reads_.push(
      ExpectedRead(data.data(), data.size(), async, data.size()));
}

void MockServiceWorkerResponseReader::ExpectReadOk(
    const std::vector<std::string>& stored_data,
    const size_t bytes_stored,
    const bool async) {
  ExpectReadInfoOk(bytes_stored, async);
  for (const auto& data : stored_data)
    ExpectReadDataOk(data, async);
}

void MockServiceWorkerResponseReader::CompletePendingRead() {
  DCHECK(!expected_reads_.empty());
  ExpectedRead expected = expected_reads_.front();
  expected_reads_.pop();
  EXPECT_TRUE(expected.async);
  if (expected.info) {
    pending_info_->response_data_size = expected.len;
  } else {
    size_t to_read = std::min(pending_buffer_len_, expected.len);
    if (to_read > 0)
      memcpy(pending_buffer_->data(), expected.data, to_read);
  }
  pending_info_ = nullptr;
  pending_buffer_ = nullptr;
  OnceCompletionCallback callback = std::move(pending_callback_);
  pending_callback_.Reset();
  std::move(callback).Run(expected.result);
}

MockServiceWorkerResponseWriter::MockServiceWorkerResponseWriter()
    : ServiceWorkerResponseWriter(/*resource_id=*/0, /*disk_cache=*/nullptr),
      info_written_(0),
      data_written_(0) {}

MockServiceWorkerResponseWriter::~MockServiceWorkerResponseWriter() = default;

void MockServiceWorkerResponseWriter::WriteInfo(
    HttpResponseInfoIOBuffer* info_buf,
    OnceCompletionCallback callback) {
  DCHECK(!expected_writes_.empty());
  ExpectedWrite write = expected_writes_.front();
  EXPECT_TRUE(write.is_info);
  if (write.result > 0) {
    EXPECT_EQ(write.length, static_cast<size_t>(info_buf->response_data_size));
    info_written_ += info_buf->response_data_size;
  }
  if (!write.async) {
    expected_writes_.pop();
    std::move(callback).Run(write.result);
  } else {
    pending_callback_ = std::move(callback);
  }
}

void MockServiceWorkerResponseWriter::WriteData(
    net::IOBuffer* buf,
    int buf_len,
    OnceCompletionCallback callback) {
  DCHECK(!expected_writes_.empty());
  ExpectedWrite write = expected_writes_.front();
  EXPECT_FALSE(write.is_info);
  if (write.result > 0) {
    EXPECT_EQ(write.length, static_cast<size_t>(buf_len));
    data_written_ += buf_len;
  }
  if (!write.async) {
    expected_writes_.pop();
    std::move(callback).Run(write.result);
  } else {
    pending_callback_ = std::move(callback);
  }
}

void MockServiceWorkerResponseWriter::ExpectWriteInfoOk(size_t length,
                                                        bool async) {
  ExpectWriteInfo(length, async, length);
}

void MockServiceWorkerResponseWriter::ExpectWriteDataOk(size_t length,
                                                        bool async) {
  ExpectWriteData(length, async, length);
}

void MockServiceWorkerResponseWriter::ExpectWriteInfo(size_t length,
                                                      bool async,
                                                      int result) {
  DCHECK_NE(net::ERR_IO_PENDING, result);
  ExpectedWrite expected(true, length, async, result);
  expected_writes_.push(expected);
}

void MockServiceWorkerResponseWriter::ExpectWriteData(size_t length,
                                                      bool async,
                                                      int result) {
  DCHECK_NE(net::ERR_IO_PENDING, result);
  ExpectedWrite expected(false, length, async, result);
  expected_writes_.push(expected);
}

void MockServiceWorkerResponseWriter::CompletePendingWrite() {
  DCHECK(!expected_writes_.empty());
  ExpectedWrite write = expected_writes_.front();
  DCHECK(write.async);
  expected_writes_.pop();
  std::move(pending_callback_).Run(write.result);
}

}  // namespace content

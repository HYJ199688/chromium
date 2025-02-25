// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_proxy_connect_job.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "base/metrics/field_trial.h"
#include "base/metrics/field_trial_param_associator.h"
#include "base/metrics/field_trial_params.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_task_environment.h"
#include "build/build_config.h"
#include "net/base/host_port_pair.h"
#include "net/base/test_proxy_delegate.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_network_session.h"
#include "net/nqe/network_quality_estimator_test_util.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/connect_job_test_util.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/socks_connect_job.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/ssl_connect_job.h"
#include "net/socket/transport_connect_job.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/test/gtest_util.h"
#include "net/test/test_with_scoped_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

const char kEndpointHost[] = "www.endpoint.test";

enum HttpProxyType { HTTP, HTTPS, SPDY };

const char kHttpProxyHost[] = "httpproxy.example.test";
const char kHttpsProxyHost[] = "httpsproxy.example.test";

}  // namespace

class HttpProxyConnectJobTest : public ::testing::TestWithParam<HttpProxyType>,
                                public WithScopedTaskEnvironment {
 protected:
  HttpProxyConnectJobTest()
      : WithScopedTaskEnvironment(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME,
            base::test::ScopedTaskEnvironment::NowSource::
                MAIN_THREAD_MOCK_TIME),
        field_trial_list_(nullptr) {
    // Set an initial delay to ensure that calls to TimeTicks::Now() do not
    // return a null value.
    FastForwardBy(base::TimeDelta::FromSeconds(1));

    // Used a mock HostResolver that does not have a cache.
    session_deps_.host_resolver = std::make_unique<MockHostResolver>();

    network_quality_estimator_ =
        std::make_unique<TestNetworkQualityEstimator>();
    session_ = SpdySessionDependencies::SpdyCreateSession(&session_deps_);
    InitCommonConnectJobParams();
  }

  virtual ~HttpProxyConnectJobTest() {
    // Reset global field trial parameters to defaults values.
    base::FieldTrialParamAssociator::GetInstance()->ClearAllParamsForTesting();
    HttpProxyConnectJob::UpdateFieldTrialParametersForTesting();
  }

  // Initializes the field trial parameters for the field trial that determines
  // connection timeout based on the network quality.
  void InitAdaptiveTimeoutFieldTrialWithParams(
      bool use_default_params,
      int ssl_http_rtt_multiplier,
      int non_ssl_http_rtt_multiplier,
      base::TimeDelta min_proxy_connection_timeout,
      base::TimeDelta max_proxy_connection_timeout) {
    std::string trial_name = "NetAdaptiveProxyConnectionTimeout";
    std::string group_name = "GroupName";

    std::map<std::string, std::string> params;
    if (!use_default_params) {
      params["ssl_http_rtt_multiplier"] =
          base::NumberToString(ssl_http_rtt_multiplier);
      params["non_ssl_http_rtt_multiplier"] =
          base::NumberToString(non_ssl_http_rtt_multiplier);
      params["min_proxy_connection_timeout_seconds"] =
          base::NumberToString(min_proxy_connection_timeout.InSeconds());
      params["max_proxy_connection_timeout_seconds"] =
          base::NumberToString(max_proxy_connection_timeout.InSeconds());
    }
    base::FieldTrialParamAssociator::GetInstance()->ClearAllParamsForTesting();
    EXPECT_TRUE(
        base::AssociateFieldTrialParams(trial_name, group_name, params));
    EXPECT_TRUE(base::FieldTrialList::CreateFieldTrial(trial_name, group_name));

    // Force static global that reads the field trials to update.
    HttpProxyConnectJob::UpdateFieldTrialParametersForTesting();
  }

  scoped_refptr<TransportSocketParams> CreateHttpProxyParams() const {
    if (GetParam() != HTTP)
      return nullptr;
    return base::MakeRefCounted<TransportSocketParams>(
        HostPortPair(kHttpProxyHost, 80), OnHostResolutionCallback());
  }

  scoped_refptr<SSLSocketParams> CreateHttpsProxyParams() const {
    if (GetParam() == HTTP)
      return nullptr;
    return base::MakeRefCounted<SSLSocketParams>(
        base::MakeRefCounted<TransportSocketParams>(
            HostPortPair(kHttpsProxyHost, 443), OnHostResolutionCallback()),
        nullptr, nullptr, HostPortPair(kHttpsProxyHost, 443), SSLConfig(),
        PRIVACY_MODE_DISABLED);
  }

  // Returns a correctly constructed HttpProxyParams for the HTTP or HTTPS
  // proxy.
  scoped_refptr<HttpProxySocketParams> CreateParams(bool tunnel) {
    return base::MakeRefCounted<HttpProxySocketParams>(
        CreateHttpProxyParams(), CreateHttpsProxyParams(), false /* is_quic */,
        HostPortPair(kEndpointHost, tunnel ? 443 : 80),
        /*is_trusted_proxy=*/false, tunnel, TRAFFIC_ANNOTATION_FOR_TESTS);
  }

  std::unique_ptr<HttpProxyConnectJob> CreateConnectJobForHttpRequest(
      ConnectJob::Delegate* delegate,
      RequestPriority priority = DEFAULT_PRIORITY) {
    return CreateConnectJob(CreateParams(false /* tunnel */), delegate,
                            priority);
  }

  std::unique_ptr<HttpProxyConnectJob> CreateConnectJobForTunnel(
      ConnectJob::Delegate* delegate,
      RequestPriority priority = DEFAULT_PRIORITY) {
    return CreateConnectJob(CreateParams(true /* tunnel */), delegate,
                            priority);
  }

  std::unique_ptr<HttpProxyConnectJob> CreateConnectJob(
      scoped_refptr<HttpProxySocketParams> http_proxy_socket_params,
      ConnectJob::Delegate* delegate,
      RequestPriority priority) {
    return std::make_unique<HttpProxyConnectJob>(
        priority, SocketTag(), common_connect_job_params_.get(),
        std::move(http_proxy_socket_params), delegate, nullptr /* net_log */);
  }

  // This may only be called at the start of the test, before any ConnectJobs
  // have been created.
  void InitProxyDelegate() {
    proxy_delegate_ = std::make_unique<TestProxyDelegate>();
    InitCommonConnectJobParams();
  }

  // This may only be called at the start of the test, before any ConnectJobs
  // have been created.
  void InitCommonConnectJobParams() {
    common_connect_job_params_ = std::make_unique<CommonConnectJobParams>(
        session_->CreateCommonConnectJobParams());
    // TODO(mmenke): Consider reworking this so it can be done through
    // |session_deps_|.
    common_connect_job_params_->proxy_delegate = proxy_delegate_.get();
    common_connect_job_params_->network_quality_estimator =
        network_quality_estimator_.get();
  }

  void Initialize(base::span<const MockRead> reads,
                  base::span<const MockWrite> writes,
                  base::span<const MockRead> spdy_reads,
                  base::span<const MockWrite> spdy_writes,
                  IoMode connect_and_ssl_io_mode) {
    if (GetParam() == SPDY) {
      data_ = std::make_unique<SequencedSocketData>(spdy_reads, spdy_writes);
    } else {
      data_ = std::make_unique<SequencedSocketData>(reads, writes);
    }

    data_->set_connect_data(MockConnect(connect_and_ssl_io_mode, OK));

    session_deps_.socket_factory->AddSocketDataProvider(data_.get());

    if (GetParam() != HTTP) {
      ssl_data_ =
          std::make_unique<SSLSocketDataProvider>(connect_and_ssl_io_mode, OK);
      if (GetParam() == SPDY) {
        InitializeSpdySsl(ssl_data_.get());
      }
      session_deps_.socket_factory->AddSSLSocketDataProvider(ssl_data_.get());
    }
  }

  void InitializeSpdySsl(SSLSocketDataProvider* ssl_data) {
    ssl_data->next_proto = kProtoHTTP2;
  }

  base::TimeDelta GetProxyConnectionTimeout() {
    // Doesn't actually matter whether or not this is for a tunnel - the
    // connection timeout is the same, though it probably shouldn't be the same,
    // since tunnels need an extra round trip.
    return HttpProxyConnectJob::ConnectionTimeout(
        *CreateParams(true /* tunnel */), network_quality_estimator_.get());
  }

 protected:
  std::unique_ptr<TestProxyDelegate> proxy_delegate_;

  std::unique_ptr<SSLSocketDataProvider> ssl_data_;
  std::unique_ptr<SequencedSocketData> data_;
  SpdySessionDependencies session_deps_;

  std::unique_ptr<TestNetworkQualityEstimator> network_quality_estimator_;

  std::unique_ptr<HttpNetworkSession> session_;

  base::FieldTrialList field_trial_list_;

  SpdyTestUtil spdy_util_;

  TestCompletionCallback callback_;

  std::unique_ptr<CommonConnectJobParams> common_connect_job_params_;
};

// All tests are run with three different proxy types: HTTP, HTTPS (non-SPDY)
// and SPDY.
INSTANTIATE_TEST_SUITE_P(HttpProxyType,
                         HttpProxyConnectJobTest,
                         ::testing::Values(HTTP, HTTPS, SPDY));

TEST_P(HttpProxyConnectJobTest, NoTunnel) {
  InitProxyDelegate();
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);
    base::HistogramTester histogram_tester;

    Initialize(base::span<MockRead>(), base::span<MockWrite>(),
               base::span<MockRead>(), base::span<MockWrite>(), io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForHttpRequest(&test_delegate);
    test_delegate.StartJobExpectingResult(connect_job.get(), OK,
                                          io_mode == SYNCHRONOUS);
    EXPECT_FALSE(proxy_delegate_->on_before_tunnel_request_called());

    bool is_secure_proxy = GetParam() == HTTPS || GetParam() == SPDY;
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Insecure.Success",
        is_secure_proxy ? 0 : 1);
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Secure.Success", is_secure_proxy ? 1 : 0);
  }
}

// Pauses an HttpProxyConnectJob at various states, and check the value of
// HasEstablishedConnection().
TEST_P(HttpProxyConnectJobTest, HasEstablishedConnectionNoTunnel) {
  session_deps_.host_resolver->set_ondemand_mode(true);

  SequencedSocketData data;
  data.set_connect_data(MockConnect(ASYNC, OK));
  session_deps_.socket_factory->AddSocketDataProvider(&data);

  // Set up SSL, if needed.
  SSLSocketDataProvider ssl_data(ASYNC, OK);
  switch (GetParam()) {
    case HTTP:
      // No SSL needed.
      break;
    case HTTPS:
      // SSL negotiation is the last step in non-tunnel connections over HTTPS
      // proxies, so pause there, to check the final state before completion.
      ssl_data = SSLSocketDataProvider(SYNCHRONOUS, ERR_IO_PENDING);
      session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);
      break;
    case SPDY:
      InitializeSpdySsl(&ssl_data);
      session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);
      break;
  }

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> connect_job =
      CreateConnectJobForHttpRequest(&test_delegate);

  // Connecting should run until the request hits the HostResolver.
  EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_TRUE(session_deps_.host_resolver->has_pending_requests());
  EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, connect_job->GetLoadState());
  EXPECT_FALSE(connect_job->HasEstablishedConnection());

  // Once the HostResolver completes, the job should start establishing a
  // connection, which will complete asynchronously.
  session_deps_.host_resolver->ResolveOnlyRequestNow();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_CONNECTING, connect_job->GetLoadState());
  EXPECT_FALSE(connect_job->HasEstablishedConnection());

  switch (GetParam()) {
    case HTTP:
    case SPDY:
      // Connection completes. Since no tunnel is established, the socket is
      // returned immediately, and HasEstablishedConnection() is only specified
      // to work before the ConnectJob completes.
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());
      break;
    case HTTPS:
      base::RunLoop().RunUntilIdle();
      EXPECT_FALSE(test_delegate.has_result());
      EXPECT_EQ(LOAD_STATE_SSL_HANDSHAKE, connect_job->GetLoadState());
      EXPECT_TRUE(connect_job->HasEstablishedConnection());

      // Unfortunately, there's no API to advance the paused SSL negotiation,
      // so just end the test here.
  }
}

// Pauses an HttpProxyConnectJob at various states, and check the value of
// HasEstablishedConnection().
TEST_P(HttpProxyConnectJobTest, HasEstablishedConnectionTunnel) {
  session_deps_.host_resolver->set_ondemand_mode(true);

  // HTTP proxy CONNECT request / response, with a pause during the read.
  MockWrite http1_writes[] = {
      MockWrite(ASYNC, 0,
                "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                "Host: www.endpoint.test:443\r\n"
                "Proxy-Connection: keep-alive\r\n\r\n"),
  };
  MockRead http1_reads[] = {
      // Pause at first read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      MockRead(ASYNC, 2, "HTTP/1.1 200 Connection Established\r\n\r\n"),
  };
  SequencedSocketData http1_data(http1_reads, http1_writes);
  http1_data.set_connect_data(MockConnect(ASYNC, OK));

  // SPDY proxy CONNECT request / response, with a pause during the read.
  spdy::SpdySerializedFrame req(spdy_util_.ConstructSpdyConnect(
      nullptr, 0, 1, HttpProxyConnectJob::kH2QuicTunnelPriority,
      HostPortPair(kEndpointHost, 443)));
  MockWrite spdy_writes[] = {CreateMockWrite(req, 0)};
  spdy::SpdySerializedFrame resp(
      spdy_util_.ConstructSpdyGetReply(nullptr, 0, 1));
  MockRead spdy_reads[] = {
      // Pause at first read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      CreateMockRead(resp, 2, ASYNC),
      MockRead(ASYNC, 0, 3),
  };
  SequencedSocketData spdy_data(spdy_reads, spdy_writes);
  spdy_data.set_connect_data(MockConnect(ASYNC, OK));

  // Will point to either the HTTP/1.x or SPDY data, depending on GetParam().
  SequencedSocketData* sequenced_data = nullptr;

  SSLSocketDataProvider ssl_data(ASYNC, OK);

  switch (GetParam()) {
    case HTTP:
      sequenced_data = &http1_data;
      break;
    case HTTPS:
      sequenced_data = &http1_data;
      session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);
      break;
    case SPDY:
      sequenced_data = &spdy_data;
      InitializeSpdySsl(&ssl_data);
      session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);
      break;
  }

  session_deps_.socket_factory->AddSocketDataProvider(sequenced_data);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> connect_job =
      CreateConnectJobForTunnel(&test_delegate);

  // Connecting should run until the request hits the HostResolver.
  EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_TRUE(session_deps_.host_resolver->has_pending_requests());
  EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, connect_job->GetLoadState());
  EXPECT_FALSE(connect_job->HasEstablishedConnection());

  // Once the HostResolver completes, the job should start establishing a
  // connection, which will complete asynchronously.
  session_deps_.host_resolver->ResolveOnlyRequestNow();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_CONNECTING, connect_job->GetLoadState());
  EXPECT_FALSE(connect_job->HasEstablishedConnection());

  // Run until the socket starts reading the proxy's handshake response.
  sequenced_data->RunUntilPaused();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_ESTABLISHING_PROXY_TUNNEL, connect_job->GetLoadState());
  EXPECT_TRUE(connect_job->HasEstablishedConnection());

  // Finish the read, and run the job until it's complete.
  sequenced_data->Resume();
  EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());
}

TEST_P(HttpProxyConnectJobTest, ProxyDelegateExtraHeaders) {
  // TODO(https://crbug.com/926427): The ProxyDelegate API is currently broken
  // in the SPDY case.
  if (GetParam() == SPDY)
    return;

  InitProxyDelegate();

  ProxyServer proxy_server(
      GetParam() == HTTP ? ProxyServer::SCHEME_HTTP : ProxyServer::SCHEME_HTTPS,
      HostPortPair(GetParam() == HTTP ? kHttpProxyHost : kHttpsProxyHost,
                   GetParam() == HTTP ? 80 : 443));
  std::string request =
      "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
      "Host: www.endpoint.test:443\r\n"
      "Proxy-Connection: keep-alive\r\n"
      "Foo: " +
      proxy_server.ToURI() + "\r\n\r\n";
  MockWrite writes[] = {
      MockWrite(ASYNC, 0, request.c_str()),
  };

  const char kResponseHeaderName[] = "Foo";
  const char kResponseHeaderValue[] = "Response";
  std::string response = base::StringPrintf(
      "HTTP/1.1 200 Connection Established\r\n"
      "%s: %s\r\n\r\n",
      kResponseHeaderName, kResponseHeaderValue);
  MockRead reads[] = {
      MockRead(ASYNC, 1, response.c_str()),
  };

  Initialize(reads, writes, base::span<MockRead>(), base::span<MockWrite>(),
             ASYNC);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> connect_job =
      CreateConnectJobForTunnel(&test_delegate);
  test_delegate.StartJobExpectingResult(connect_job.get(), OK,
                                        false /* expect_sync_result */);
  proxy_delegate_->VerifyOnHttp1TunnelHeadersReceived(
      proxy_server, kResponseHeaderName, kResponseHeaderValue);
}

// Test the case where auth credentials are not cached.
TEST_P(HttpProxyConnectJobTest, NeedAuth) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);

    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
        MockWrite(io_mode, 5,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };
    MockRead reads[] = {
        // No credentials.
        MockRead(io_mode, 1, "HTTP/1.1 407 Proxy Authentication Required\r\n"),
        MockRead(io_mode, 2,
                 "Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
        MockRead(io_mode, 3, "Content-Length: 10\r\n\r\n"),
        MockRead(io_mode, 4, "0123456789"),
        MockRead(io_mode, 6, "HTTP/1.1 200 Connection Established\r\n\r\n"),
    };

    SpdyTestUtil spdy_util;
    spdy::SpdySerializedFrame connect(spdy_util.ConstructSpdyConnect(
        nullptr, 0, 1, HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));
    spdy::SpdySerializedFrame rst(
        spdy_util.ConstructSpdyRstStream(1, spdy::ERROR_CODE_CANCEL));
    spdy_util.UpdateWithStreamDestruction(1);

    // After calling trans.RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    const char* const kSpdyAuthCredentials[] = {
        "proxy-authorization",
        "Basic Zm9vOmJhcg==",
    };
    spdy::SpdySerializedFrame connect2(spdy_util.ConstructSpdyConnect(
        kSpdyAuthCredentials, base::size(kSpdyAuthCredentials) / 2, 3,
        HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));

    MockWrite spdy_writes[] = {
        CreateMockWrite(connect, 0, io_mode),
        CreateMockWrite(rst, 2, io_mode),
        CreateMockWrite(connect2, 3, io_mode),
    };

    // The proxy responds to the connect with a 407, using a persistent
    // connection.
    const char kAuthStatus[] = "407";
    const char* const kAuthChallenge[] = {
        "proxy-authenticate",
        "Basic realm=\"MyRealm1\"",
    };
    spdy::SpdySerializedFrame connect_auth_resp(
        spdy_util.ConstructSpdyReplyError(kAuthStatus, kAuthChallenge,
                                          base::size(kAuthChallenge) / 2, 1));

    spdy::SpdySerializedFrame connect2_resp(
        spdy_util.ConstructSpdyGetReply(nullptr, 0, 3));
    MockRead spdy_reads[] = {
        CreateMockRead(connect_auth_resp, 1, ASYNC),
        CreateMockRead(connect2_resp, 4, ASYNC),
        MockRead(ASYNC, OK, 5),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    ASSERT_EQ(ERR_IO_PENDING, connect_job->Connect());
    // Auth callback is always invoked asynchronously when a challenge is
    // observed.
    EXPECT_EQ(0, test_delegate.num_auth_challenges());

    test_delegate.WaitForAuthChallenge(1);
    ASSERT_TRUE(test_delegate.auth_response_info().headers);
    EXPECT_EQ(407, test_delegate.auth_response_info().headers->response_code());
    std::string proxy_authenticate;
    ASSERT_TRUE(test_delegate.auth_response_info().headers->EnumerateHeader(
        nullptr, "Proxy-Authenticate", &proxy_authenticate));
    EXPECT_EQ(proxy_authenticate, "Basic realm=\"MyRealm1\"");
    ASSERT_TRUE(test_delegate.auth_controller());
    EXPECT_FALSE(test_delegate.has_result());

    test_delegate.auth_controller()->ResetAuth(
        AuthCredentials(base::ASCIIToUTF16("foo"), base::ASCIIToUTF16("bar")));
    test_delegate.RunAuthCallback();
    // Per API contract, the request can not complete synchronously.
    EXPECT_FALSE(test_delegate.has_result());

    EXPECT_EQ(net::OK, test_delegate.WaitForResult());
    EXPECT_EQ(1, test_delegate.num_auth_challenges());

    // Close the H2 session to prevent reuse.
    if (GetParam() == SPDY)
      session_->CloseAllConnections();
    // Also need to clear the auth cache before re-running the test.
    session_->http_auth_cache()->ClearAllEntries();
  }
}

// Test the case where auth credentials are not cached and the first time
// credentials are sent, they are rejected.
TEST_P(HttpProxyConnectJobTest, NeedAuthTwice) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);

    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
        MockWrite(io_mode, 2,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
        MockWrite(io_mode, 4,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };
    MockRead reads[] = {
        // No credentials.
        MockRead(io_mode, 1,
                 "HTTP/1.1 407 Proxy Authentication Required\r\n"
                 "Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"
                 "Content-Length: 0\r\n\r\n"),
        MockRead(io_mode, 3,
                 "HTTP/1.1 407 Proxy Authentication Required\r\n"
                 "Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"
                 "Content-Length: 0\r\n\r\n"),
        MockRead(io_mode, 5, "HTTP/1.1 200 Connection Established\r\n\r\n"),
    };

    SpdyTestUtil spdy_util;
    spdy::SpdySerializedFrame connect(spdy_util.ConstructSpdyConnect(
        nullptr, 0, 1, HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));
    spdy::SpdySerializedFrame rst(
        spdy_util.ConstructSpdyRstStream(1, spdy::ERROR_CODE_CANCEL));
    spdy_util.UpdateWithStreamDestruction(1);

    // After calling trans.RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    const char* const kSpdyAuthCredentials[] = {
        "proxy-authorization",
        "Basic Zm9vOmJhcg==",
    };
    spdy::SpdySerializedFrame connect2(spdy_util.ConstructSpdyConnect(
        kSpdyAuthCredentials, base::size(kSpdyAuthCredentials) / 2, 3,
        HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));
    spdy::SpdySerializedFrame rst2(
        spdy_util.ConstructSpdyRstStream(3, spdy::ERROR_CODE_CANCEL));
    spdy_util.UpdateWithStreamDestruction(3);

    spdy::SpdySerializedFrame connect3(spdy_util.ConstructSpdyConnect(
        kSpdyAuthCredentials, base::size(kSpdyAuthCredentials) / 2, 5,
        HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));
    MockWrite spdy_writes[] = {
        CreateMockWrite(connect, 0, io_mode),
        CreateMockWrite(rst, 2, io_mode),
        CreateMockWrite(connect2, 3, io_mode),
        CreateMockWrite(rst2, 5, io_mode),
        CreateMockWrite(connect3, 6, io_mode),
    };

    // The proxy responds to the connect with a 407, using a persistent
    // connection.
    const char kAuthStatus[] = "407";
    const char* const kAuthChallenge[] = {
        "proxy-authenticate",
        "Basic realm=\"MyRealm1\"",
    };
    spdy::SpdySerializedFrame connect_auth_resp(
        spdy_util.ConstructSpdyReplyError(kAuthStatus, kAuthChallenge,
                                          base::size(kAuthChallenge) / 2, 1));
    spdy::SpdySerializedFrame connect2_auth_resp(
        spdy_util.ConstructSpdyReplyError(kAuthStatus, kAuthChallenge,
                                          base::size(kAuthChallenge) / 2, 3));
    spdy::SpdySerializedFrame connect3_resp(
        spdy_util.ConstructSpdyGetReply(nullptr, 0, 5));
    MockRead spdy_reads[] = {
        CreateMockRead(connect_auth_resp, 1, ASYNC),
        CreateMockRead(connect2_auth_resp, 4, ASYNC),
        CreateMockRead(connect3_resp, 7, ASYNC),
        MockRead(ASYNC, OK, 8),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    ASSERT_EQ(ERR_IO_PENDING, connect_job->Connect());
    // Auth callback is always invoked asynchronously when a challenge is
    // observed.
    EXPECT_EQ(0, test_delegate.num_auth_challenges());

    test_delegate.WaitForAuthChallenge(1);
    ASSERT_TRUE(test_delegate.auth_response_info().headers);
    EXPECT_EQ(407, test_delegate.auth_response_info().headers->response_code());
    std::string proxy_authenticate;
    ASSERT_TRUE(test_delegate.auth_response_info().headers->EnumerateHeader(
        nullptr, "Proxy-Authenticate", &proxy_authenticate));
    EXPECT_EQ(proxy_authenticate, "Basic realm=\"MyRealm1\"");
    EXPECT_FALSE(test_delegate.has_result());

    test_delegate.auth_controller()->ResetAuth(
        AuthCredentials(base::ASCIIToUTF16("foo"), base::ASCIIToUTF16("bar")));
    test_delegate.RunAuthCallback();
    // Per API contract, the auth callback can't be invoked synchronously.
    EXPECT_FALSE(test_delegate.auth_controller());
    EXPECT_FALSE(test_delegate.has_result());

    test_delegate.WaitForAuthChallenge(2);
    ASSERT_TRUE(test_delegate.auth_response_info().headers);
    EXPECT_EQ(407, test_delegate.auth_response_info().headers->response_code());
    ASSERT_TRUE(test_delegate.auth_response_info().headers->EnumerateHeader(
        nullptr, "Proxy-Authenticate", &proxy_authenticate));
    EXPECT_EQ(proxy_authenticate, "Basic realm=\"MyRealm1\"");
    EXPECT_FALSE(test_delegate.has_result());

    test_delegate.auth_controller()->ResetAuth(
        AuthCredentials(base::ASCIIToUTF16("foo"), base::ASCIIToUTF16("bar")));
    test_delegate.RunAuthCallback();
    // Per API contract, the request can't complete synchronously.
    EXPECT_FALSE(test_delegate.has_result());

    EXPECT_EQ(net::OK, test_delegate.WaitForResult());
    EXPECT_EQ(2, test_delegate.num_auth_challenges());

    // Close the H2 session to prevent reuse.
    if (GetParam() == SPDY)
      session_->CloseAllConnections();
    // Also need to clear the auth cache before re-running the test.
    session_->http_auth_cache()->ClearAllEntries();
  }
}

// Test the case where auth credentials are cached.
TEST_P(HttpProxyConnectJobTest, HaveAuth) {
  // Prepopulate auth cache.
  const base::string16 kFoo(base::ASCIIToUTF16("foo"));
  const base::string16 kBar(base::ASCIIToUTF16("bar"));
  GURL proxy_url(GetParam() == HTTP
                     ? (std::string("http://") + kHttpProxyHost)
                     : (std::string("https://") + kHttpsProxyHost));
  session_->http_auth_cache()->Add(
      proxy_url, "MyRealm1", HttpAuth::AUTH_SCHEME_BASIC,
      "Basic realm=MyRealm1", AuthCredentials(kFoo, kBar), "/");

  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);

    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, 1, "HTTP/1.1 200 Connection Established\r\n\r\n"),
    };

    const char* const kSpdyAuthCredentials[] = {
        "proxy-authorization",
        "Basic Zm9vOmJhcg==",
    };
    SpdyTestUtil spdy_util;
    spdy::SpdySerializedFrame connect(spdy_util.ConstructSpdyConnect(
        kSpdyAuthCredentials, base::size(kSpdyAuthCredentials) / 2, 1,
        HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));

    MockWrite spdy_writes[] = {
        CreateMockWrite(connect, 0, ASYNC),
    };

    spdy::SpdySerializedFrame connect_resp(
        spdy_util.ConstructSpdyGetReply(nullptr, 0, 1));
    MockRead spdy_reads[] = {
        // SpdySession starts trying to read from the socket as soon as it's
        // created, so this cannot be SYNCHRONOUS.
        CreateMockRead(connect_resp, 1, ASYNC),
        MockRead(SYNCHRONOUS, ERR_IO_PENDING, 2),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    // SPDY operations always complete asynchronously.
    test_delegate.StartJobExpectingResult(
        connect_job.get(), OK, io_mode == SYNCHRONOUS && GetParam() != SPDY);

    // Close the H2 session to prevent reuse.
    if (GetParam() == SPDY)
      session_->CloseAllConnections();
  }
}

TEST_P(HttpProxyConnectJobTest, RequestPriority) {
  // Make request hang during host resolution, so can observe priority there.
  session_deps_.host_resolver->set_ondemand_mode(true);

  for (int initial_priority = MINIMUM_PRIORITY;
       initial_priority <= MAXIMUM_PRIORITY; ++initial_priority) {
    SCOPED_TRACE(initial_priority);
    for (int new_priority = MINIMUM_PRIORITY; new_priority <= MAXIMUM_PRIORITY;
         ++new_priority) {
      SCOPED_TRACE(new_priority);
      if (initial_priority == new_priority)
        continue;
      TestConnectJobDelegate test_delegate;
      std::unique_ptr<ConnectJob> connect_job = CreateConnectJobForHttpRequest(
          &test_delegate, static_cast<RequestPriority>(initial_priority));
      EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
      EXPECT_FALSE(test_delegate.has_result());

      MockHostResolverBase* host_resolver = session_deps_.host_resolver.get();
      size_t request_id = host_resolver->last_id();
      EXPECT_EQ(initial_priority, host_resolver->request_priority(request_id));

      connect_job->ChangePriority(static_cast<RequestPriority>(new_priority));
      EXPECT_EQ(new_priority, host_resolver->request_priority(request_id));

      connect_job->ChangePriority(
          static_cast<RequestPriority>(initial_priority));
      EXPECT_EQ(initial_priority, host_resolver->request_priority(request_id));
    }
  }
}

// Make sure that HttpProxyConnectJob does not pass on its priority to its
// SPDY session's socket request on Init, or on SetPriority.
TEST_P(HttpProxyConnectJobTest, SetSpdySessionSocketRequestPriority) {
  if (GetParam() != SPDY)
    return;
  session_deps_.host_resolver->set_synchronous_mode(true);

  // The SPDY CONNECT request should have a priority of kH2QuicTunnelPriority,
  // even though the ConnectJob's priority is set to HIGHEST after connection
  // establishment.
  spdy::SpdySerializedFrame req(spdy_util_.ConstructSpdyConnect(
      nullptr /* extra_headers */, 0 /* extra_header_count */,
      1 /* stream_id */, HttpProxyConnectJob::kH2QuicTunnelPriority,
      HostPortPair(kEndpointHost, 443)));
  MockWrite spdy_writes[] = {CreateMockWrite(req, 0, ASYNC)};
  spdy::SpdySerializedFrame resp(
      spdy_util_.ConstructSpdyGetReply(nullptr, 0, 1));
  MockRead spdy_reads[] = {CreateMockRead(resp, 1, ASYNC),
                           MockRead(ASYNC, 0, 2)};

  Initialize(base::span<MockRead>(), base::span<MockWrite>(), spdy_reads,
             spdy_writes, SYNCHRONOUS);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> connect_job =
      CreateConnectJobForTunnel(&test_delegate, IDLE);
  EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_FALSE(test_delegate.has_result());

  connect_job->ChangePriority(HIGHEST);

  // Wait for tunnel to be established. If the frame has a MEDIUM priority
  // instead of highest, the written data will not match what is expected, and
  // the test will fail.
  EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());
}

TEST_P(HttpProxyConnectJobTest, TCPError) {
  // SPDY and HTTPS are identical, as they only differ once a connection is
  // established.
  if (GetParam() == SPDY)
    return;
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);
    base::HistogramTester histogram_tester;

    SequencedSocketData data;
    data.set_connect_data(MockConnect(io_mode, ERR_CONNECTION_CLOSED));
    session_deps_.socket_factory->AddSocketDataProvider(&data);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForHttpRequest(&test_delegate);
    test_delegate.StartJobExpectingResult(
        connect_job.get(), ERR_PROXY_CONNECTION_FAILED, io_mode == SYNCHRONOUS);

    bool is_secure_proxy = GetParam() == HTTPS;
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Insecure.Error", is_secure_proxy ? 0 : 1);
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Secure.Error", is_secure_proxy ? 1 : 0);
  }
}

TEST_P(HttpProxyConnectJobTest, SSLError) {
  if (GetParam() == HTTP)
    return;

  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);
    base::HistogramTester histogram_tester;

    SequencedSocketData data;
    data.set_connect_data(MockConnect(io_mode, OK));
    session_deps_.socket_factory->AddSocketDataProvider(&data);

    SSLSocketDataProvider ssl_data(io_mode, ERR_CERT_AUTHORITY_INVALID);
    if (GetParam() == SPDY) {
      InitializeSpdySsl(&ssl_data);
    }
    session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    test_delegate.StartJobExpectingResult(connect_job.get(),
                                          ERR_PROXY_CERTIFICATE_INVALID,
                                          io_mode == SYNCHRONOUS);

    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Secure.Error", 1);
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Insecure.Error", 0);
  }
}

TEST_P(HttpProxyConnectJobTest, TunnelUnexpectedClose) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, 1, "HTTP/1.1 200 Conn"),
        MockRead(io_mode, ERR_CONNECTION_CLOSED, 2),
    };
    spdy::SpdySerializedFrame req(SpdyTestUtil().ConstructSpdyConnect(
        nullptr /*extra_headers */, 0 /*extra_header_count */,
        1 /* stream_id */, HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair(kEndpointHost, 443)));
    MockWrite spdy_writes[] = {CreateMockWrite(req, 0, io_mode)};
    // Sync reads don't really work with SPDY, since it constantly reads from
    // the socket.
    MockRead spdy_reads[] = {
        MockRead(ASYNC, ERR_CONNECTION_CLOSED, 1),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);

    if (GetParam() == SPDY) {
      // SPDY cannot process a headers block unless it's complete and so it
      // returns ERR_CONNECTION_CLOSED in this case. SPDY also doesn't return
      // this failure synchronously.
      test_delegate.StartJobExpectingResult(connect_job.get(),
                                            ERR_CONNECTION_CLOSED,
                                            false /* expect_sync_result */);
    } else {
      test_delegate.StartJobExpectingResult(connect_job.get(),
                                            ERR_RESPONSE_HEADERS_TRUNCATED,
                                            io_mode == SYNCHRONOUS);
    }
  }
}

TEST_P(HttpProxyConnectJobTest, Tunnel1xxResponse) {
  // Tests that 1xx responses are rejected for a CONNECT request.
  if (GetParam() == SPDY) {
    // SPDY doesn't have 1xx responses.
    return;
  }

  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, 1, "HTTP/1.1 100 Continue\r\n\r\n"),
        MockRead(io_mode, 2, "HTTP/1.1 200 Connection Established\r\n\r\n"),
    };

    Initialize(reads, writes, base::span<MockRead>(), base::span<MockWrite>(),
               io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    test_delegate.StartJobExpectingResult(connect_job.get(),
                                          ERR_TUNNEL_CONNECTION_FAILED,
                                          io_mode == SYNCHRONOUS);
  }
}

TEST_P(HttpProxyConnectJobTest, TunnelSetupError) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, 1, "HTTP/1.1 304 Not Modified\r\n\r\n"),
    };
    SpdyTestUtil spdy_util;
    spdy::SpdySerializedFrame req(spdy_util.ConstructSpdyConnect(
        nullptr /* extra_headers */, 0 /* extra_header_count */,
        1 /* stream_id */, HttpProxyConnectJob::kH2QuicTunnelPriority,
        HostPortPair("www.endpoint.test", 443)));
    spdy::SpdySerializedFrame rst(
        spdy_util.ConstructSpdyRstStream(1, spdy::ERROR_CODE_CANCEL));
    MockWrite spdy_writes[] = {
        CreateMockWrite(req, 0, io_mode),
        CreateMockWrite(rst, 2, io_mode),
    };
    spdy::SpdySerializedFrame resp(spdy_util.ConstructSpdyReplyError(1));
    // Sync reads don't really work with SPDY, since it constantly reads from
    // the socket.
    MockRead spdy_reads[] = {
        CreateMockRead(resp, 1, ASYNC),
        MockRead(ASYNC, OK, 3),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate, LOW);
    test_delegate.StartJobExpectingResult(
        connect_job.get(), ERR_TUNNEL_CONNECTION_FAILED,
        io_mode == SYNCHRONOUS && GetParam() != SPDY);
    // Need to close the session to prevent reuse in the next loop iteration.
    session_->spdy_session_pool()->CloseAllSessions();
  }
}

TEST_P(HttpProxyConnectJobTest, SslClientAuth) {
  if (GetParam() == HTTP)
    return;
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);
    base::HistogramTester histogram_tester;

    SequencedSocketData socket_data(MockConnect(io_mode, OK),
                                    base::span<const MockRead>(),
                                    base::span<const MockWrite>());
    session_deps_.socket_factory->AddSocketDataProvider(&socket_data);
    SSLSocketDataProvider ssl_data(io_mode, ERR_SSL_CLIENT_AUTH_CERT_NEEDED);
    if (GetParam() == SPDY)
      InitializeSpdySsl(&ssl_data);
    session_deps_.socket_factory->AddSSLSocketDataProvider(&ssl_data);

    // Redirects in the HTTPS case return errors, but also return sockets.
    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);
    test_delegate.StartJobExpectingResult(connect_job.get(),
                                          ERR_SSL_CLIENT_AUTH_CERT_NEEDED,
                                          io_mode == SYNCHRONOUS);

    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Secure.Error", 1);
    histogram_tester.ExpectTotalCount(
        "Net.HttpProxy.ConnectLatency.Insecure.Error", 0);
  }
}

TEST_P(HttpProxyConnectJobTest, TunnelSetupRedirect) {
  const std::string kRedirectTarget = "https://foo.google.com/";

  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    session_deps_.host_resolver->set_synchronous_mode(io_mode == SYNCHRONOUS);

    const std::string kResponseText =
        "HTTP/1.1 302 Found\r\n"
        "Location: " +
        kRedirectTarget +
        "\r\n"
        "Set-Cookie: foo=bar\r\n"
        "\r\n";

    MockWrite writes[] = {
        MockWrite(io_mode, 0,
                  "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                  "Host: www.endpoint.test:443\r\n"
                  "Proxy-Connection: keep-alive\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, 1, kResponseText.c_str()),
    };
    SpdyTestUtil spdy_util;
    spdy::SpdySerializedFrame req(spdy_util.ConstructSpdyConnect(
        nullptr /* extra_headers */, 0 /* extra_header_count */, 1,
        DEFAULT_PRIORITY, HostPortPair(kEndpointHost, 443)));
    spdy::SpdySerializedFrame rst(
        spdy_util.ConstructSpdyRstStream(1, spdy::ERROR_CODE_CANCEL));

    MockWrite spdy_writes[] = {
        CreateMockWrite(req, 0, io_mode),
        CreateMockWrite(rst, 3, io_mode),
    };

    const char* const responseHeaders[] = {
        "location",
        kRedirectTarget.c_str(),
        "set-cookie",
        "foo=bar",
    };
    const int responseHeadersSize = base::size(responseHeaders) / 2;
    spdy::SpdySerializedFrame resp(spdy_util.ConstructSpdyReplyError(
        "302", responseHeaders, responseHeadersSize, 1));
    MockRead spdy_reads[] = {
        CreateMockRead(resp, 1, ASYNC),
        MockRead(ASYNC, 0, 2),
    };

    Initialize(reads, writes, spdy_reads, spdy_writes, io_mode);

    // Redirects in the HTTPS case return errors, but also return sockets.
    TestConnectJobDelegate test_delegate(
        GetParam() == HTTP
            ? TestConnectJobDelegate::SocketExpected::ON_SUCCESS_ONLY
            : TestConnectJobDelegate::SocketExpected::ALWAYS);
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);

    // H2 never completes synchronously.
    bool expect_sync_result = (io_mode == SYNCHRONOUS && GetParam() != SPDY);

    if (GetParam() == HTTP) {
      // We don't trust 302 responses to CONNECT from HTTP proxies.
      test_delegate.StartJobExpectingResult(
          connect_job.get(), ERR_TUNNEL_CONNECTION_FAILED, expect_sync_result);
      EXPECT_FALSE(test_delegate.socket());
    } else {
      // Expect ProxyClientSocket to return the proxy's response, sanitized.
      test_delegate.StartJobExpectingResult(
          connect_job.get(), ERR_HTTPS_PROXY_TUNNEL_RESPONSE_REDIRECT,
          expect_sync_result);
      ASSERT_TRUE(test_delegate.socket());

      const ProxyClientSocket* tunnel_socket =
          static_cast<ProxyClientSocket*>(test_delegate.socket());
      const HttpResponseInfo* response =
          tunnel_socket->GetConnectResponseInfo();
      const HttpResponseHeaders* headers = response->headers.get();

      // Make sure Set-Cookie header was stripped.
      EXPECT_FALSE(headers->HasHeader("set-cookie"));

      // Make sure Content-Length: 0 header was added.
      EXPECT_TRUE(headers->HasHeaderValue("content-length", "0"));

      // Make sure Location header was included and correct.
      std::string location;
      EXPECT_TRUE(headers->IsRedirect(&location));
      EXPECT_EQ(location, kRedirectTarget);
    }

    // Need to close the session to prevent reuse in the next loop iteration.
    session_->spdy_session_pool()->CloseAllSessions();
  }
}

// Test timeouts in the case of an auth challenge and response.
TEST_P(HttpProxyConnectJobTest, TestTimeoutsAuthChallenge) {
  // Wait until this amount of time before something times out.
  const base::TimeDelta kTinyTime = base::TimeDelta::FromMicroseconds(1);

  enum class TimeoutPhase {
    CONNECT,
    PROXY_HANDSHAKE,
    SECOND_PROXY_HANDSHAKE,

    NONE,
  };

  const TimeoutPhase kTimeoutPhases[] = {
      TimeoutPhase::CONNECT,
      TimeoutPhase::PROXY_HANDSHAKE,
      TimeoutPhase::SECOND_PROXY_HANDSHAKE,
      TimeoutPhase::NONE,
  };

  session_deps_.host_resolver->set_ondemand_mode(true);

  MockWrite writes[] = {
      MockWrite(ASYNC, 0,
                "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                "Host: www.endpoint.test:443\r\n"
                "Proxy-Connection: keep-alive\r\n\r\n"),
      MockWrite(ASYNC, 3,
                "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                "Host: www.endpoint.test:443\r\n"
                "Proxy-Connection: keep-alive\r\n"
                "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };
  MockRead reads[] = {
      // Pause before first response is read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      MockRead(ASYNC, 2,
               "HTTP/1.1 407 Proxy Authentication Required\r\n"
               "Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"
               "Content-Length: 0\r\n\r\n"),

      // Pause again before second response is read.
      MockRead(ASYNC, ERR_IO_PENDING, 4),
      MockRead(ASYNC, 5, "HTTP/1.1 200 Connection Established\r\n\r\n"),
  };

  SpdyTestUtil spdy_util;
  spdy::SpdySerializedFrame connect(spdy_util.ConstructSpdyConnect(
      nullptr, 0, 1, HttpProxyConnectJob::kH2QuicTunnelPriority,
      HostPortPair(kEndpointHost, 443)));
  spdy::SpdySerializedFrame rst(
      spdy_util.ConstructSpdyRstStream(1, spdy::ERROR_CODE_CANCEL));
  spdy_util.UpdateWithStreamDestruction(1);

  // After calling trans.RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  const char* const kSpdyAuthCredentials[] = {
      "proxy-authorization",
      "Basic Zm9vOmJhcg==",
  };
  spdy::SpdySerializedFrame connect2(spdy_util.ConstructSpdyConnect(
      kSpdyAuthCredentials, base::size(kSpdyAuthCredentials) / 2, 3,
      HttpProxyConnectJob::kH2QuicTunnelPriority,
      HostPortPair(kEndpointHost, 443)));
  // This may be sent in some tests, either when tearing down a successful
  // connection, or on timeout.
  spdy::SpdySerializedFrame rst2(
      spdy_util.ConstructSpdyRstStream(3, spdy::ERROR_CODE_CANCEL));
  MockWrite spdy_writes[] = {
      CreateMockWrite(connect, 0, ASYNC),
      CreateMockWrite(rst, 3, ASYNC),
      CreateMockWrite(connect2, 4, ASYNC),
      CreateMockWrite(rst2, 8, ASYNC),
  };

  // The proxy responds to the connect with a 407, using a persistent
  // connection.
  const char kAuthStatus[] = "407";
  const char* const kAuthChallenge[] = {
      "proxy-authenticate",
      "Basic realm=\"MyRealm1\"",
  };
  spdy::SpdySerializedFrame connect_auth_resp(spdy_util.ConstructSpdyReplyError(
      kAuthStatus, kAuthChallenge, base::size(kAuthChallenge) / 2, 1));
  spdy::SpdySerializedFrame connect2_resp(
      spdy_util.ConstructSpdyGetReply(nullptr, 0, 3));
  MockRead spdy_reads[] = {
      // Pause before first response is read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      CreateMockRead(connect_auth_resp, 2, ASYNC),
      // Pause again before second response is read.
      MockRead(ASYNC, ERR_IO_PENDING, 5),
      CreateMockRead(connect2_resp, 6, ASYNC),
      MockRead(ASYNC, OK, 7),
  };

  for (TimeoutPhase timeout_phase : kTimeoutPhases) {
    SCOPED_TRACE(static_cast<int>(timeout_phase));

    // Need to close the session to prevent reuse of a session from the last
    // loop iteration.
    session_->spdy_session_pool()->CloseAllSessions();
    // And clear the auth cache to prevent reusing cache entries.
    session_->http_auth_cache()->ClearAllEntries();

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);

    // Connecting should run until the request hits the HostResolver.
    EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_TRUE(session_deps_.host_resolver->has_pending_requests());
    EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, connect_job->GetLoadState());

    // Run until just before timeout. The proxy timeout can be less than the
    // connection timeout, as it can be set based on the
    // NetworkQualityEstimator, which the connection timeout is not.
    FastForwardBy(std::min(TransportConnectJob::ConnectionTimeout(),
                           GetProxyConnectionTimeout()) -
                  kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    // Wait until timeout, if appropriate.
    if (timeout_phase == TimeoutPhase::CONNECT) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    // Add mock reads for socket needed in next step. Connect phase is timed out
    // before establishing a connection, so don't need them for
    // TimeoutPhase::CONNECT.
    Initialize(reads, writes, spdy_reads, spdy_writes, SYNCHRONOUS);

    // Finish resolution.
    session_deps_.host_resolver->ResolveOnlyRequestNow();
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_EQ(LOAD_STATE_ESTABLISHING_PROXY_TUNNEL,
              connect_job->GetLoadState());

    // Wait until just before negotiation with the tunnel should time out.
    FastForwardBy(HttpProxyConnectJob::TunnelTimeoutForTesting() - kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    if (timeout_phase == TimeoutPhase::PROXY_HANDSHAKE) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    data_->Resume();
    test_delegate.WaitForAuthChallenge(1);
    EXPECT_FALSE(test_delegate.has_result());

    // ConnectJobs cannot timeout while showing an auth dialog.
    FastForwardBy(base::TimeDelta::FromDays(1) + GetProxyConnectionTimeout());
    EXPECT_FALSE(test_delegate.has_result());

    // Send credentials
    test_delegate.auth_controller()->ResetAuth(
        AuthCredentials(base::ASCIIToUTF16("foo"), base::ASCIIToUTF16("bar")));
    test_delegate.RunAuthCallback();
    EXPECT_FALSE(test_delegate.has_result());

    // When sending proxy auth on the same socket a challenge was just received
    // on, all subsequent proxy handshakes cannot timeout. However, H2 always
    // follows the establish new connection path, which means its second proxy
    // handshake *can* timeout. Retrying with an already established H2 stream
    // uses the entire proxy timeout for just establishing the tunnel, rather
    // than the tunnel timeout.
    //
    // TODO(https://crbug.com/937137): This behavior seems strange. Should we do
    // something about it?
    if (GetParam() == SPDY) {
      FastForwardBy(GetProxyConnectionTimeout() - kTinyTime);
      EXPECT_FALSE(test_delegate.has_result());

      if (timeout_phase == TimeoutPhase::SECOND_PROXY_HANDSHAKE) {
        FastForwardBy(kTinyTime);
        ASSERT_TRUE(test_delegate.has_result());
        EXPECT_THAT(test_delegate.WaitForResult(),
                    test::IsError(ERR_TIMED_OUT));
        continue;
      }
    } else {
      // See above comment for explanation.
      FastForwardBy(base::TimeDelta::FromDays(1) + GetProxyConnectionTimeout());
      EXPECT_FALSE(test_delegate.has_result());
    }

    data_->Resume();
    EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());
  }
}

// Same as above, except test the case the first connection cannot be reused
// once credentials are received.
TEST_P(HttpProxyConnectJobTest, TestTimeoutsAuthChallengeNewConnection) {
  // Proxy-Connection: Close doesn't make sense with H2.
  if (GetParam() == SPDY)
    return;

  enum class TimeoutPhase {
    CONNECT,
    PROXY_HANDSHAKE,
    SECOND_CONNECT,
    SECOND_PROXY_HANDSHAKE,

    // This has to be last for the H2 proxy case, since success will populate
    // the H2 session pool.
    NONE,
  };

  const TimeoutPhase kTimeoutPhases[] = {
      TimeoutPhase::CONNECT,        TimeoutPhase::PROXY_HANDSHAKE,
      TimeoutPhase::SECOND_CONNECT, TimeoutPhase::SECOND_PROXY_HANDSHAKE,
      TimeoutPhase::NONE,
  };

  // Wait until this amount of time before something times out.
  const base::TimeDelta kTinyTime = base::TimeDelta::FromMicroseconds(1);

  session_deps_.host_resolver->set_ondemand_mode(true);

  MockWrite writes[] = {
      MockWrite(ASYNC, 0,
                "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                "Host: www.endpoint.test:443\r\n"
                "Proxy-Connection: keep-alive\r\n\r\n"),
  };
  MockRead reads[] = {
      // Pause at read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      MockRead(ASYNC, 2,
               "HTTP/1.1 407 Proxy Authentication Required\r\n"
               "Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"
               "Proxy-Connection: Close\r\n"
               "Content-Length: 0\r\n\r\n"),
  };

  MockWrite writes2[] = {
      MockWrite(ASYNC, 0,
                "CONNECT www.endpoint.test:443 HTTP/1.1\r\n"
                "Host: www.endpoint.test:443\r\n"
                "Proxy-Connection: keep-alive\r\n"
                "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };
  MockRead reads2[] = {
      // Pause at read.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      MockRead(ASYNC, 2, "HTTP/1.1 200 Connection Established\r\n\r\n"),
  };

  for (TimeoutPhase timeout_phase : kTimeoutPhases) {
    SCOPED_TRACE(static_cast<int>(timeout_phase));

    // Need to clear the auth cache to prevent reusing cache entries.
    session_->http_auth_cache()->ClearAllEntries();

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> connect_job =
        CreateConnectJobForTunnel(&test_delegate);

    // Connecting should run until the request hits the HostResolver.
    EXPECT_THAT(connect_job->Connect(), test::IsError(ERR_IO_PENDING));
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_TRUE(session_deps_.host_resolver->has_pending_requests());
    EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, connect_job->GetLoadState());

    // Run until just before timeout. The proxy timeout can be less than the
    // connection timeout, as it can be set based on the
    // NetworkQualityEstimator, which the connection timeout is not.
    FastForwardBy(std::min(TransportConnectJob::ConnectionTimeout(),
                           GetProxyConnectionTimeout()) -
                  kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    // Wait until timeout, if appropriate.
    if (timeout_phase == TimeoutPhase::CONNECT) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    // Add mock reads for socket needed in next step. Connect phase is timed out
    // before establishing a connection, so don't need them for
    // TimeoutPhase::CONNECT.
    Initialize(reads, writes, base::span<MockRead>(), base::span<MockWrite>(),
               SYNCHRONOUS);

    // Finish resolution.
    session_deps_.host_resolver->ResolveOnlyRequestNow();
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_EQ(LOAD_STATE_ESTABLISHING_PROXY_TUNNEL,
              connect_job->GetLoadState());

    // Wait until just before negotiation with the tunnel should time out.
    FastForwardBy(HttpProxyConnectJob::TunnelTimeoutForTesting() - kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    if (timeout_phase == TimeoutPhase::PROXY_HANDSHAKE) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    data_->Resume();
    test_delegate.WaitForAuthChallenge(1);
    EXPECT_FALSE(test_delegate.has_result());

    // ConnectJobs cannot timeout while showing an auth dialog.
    FastForwardBy(base::TimeDelta::FromDays(1) + GetProxyConnectionTimeout());
    EXPECT_FALSE(test_delegate.has_result());

    // Send credentials
    test_delegate.auth_controller()->ResetAuth(
        AuthCredentials(base::ASCIIToUTF16("foo"), base::ASCIIToUTF16("bar")));
    test_delegate.RunAuthCallback();
    EXPECT_FALSE(test_delegate.has_result());

    // Since the connection was not reusable, a new connection needs to be
    // established.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_TRUE(session_deps_.host_resolver->has_pending_requests());
    EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, connect_job->GetLoadState());

    // Run until just before timeout. The proxy timeout can be less than the
    // connection timeout, as it can be set based on the
    // NetworkQualityEstimator, which the connection timeout is not.
    FastForwardBy(std::min(TransportConnectJob::ConnectionTimeout(),
                           GetProxyConnectionTimeout()) -
                  kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    // Wait until timeout, if appropriate.
    if (timeout_phase == TimeoutPhase::SECOND_CONNECT) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    // Add mock reads for socket needed in next step. Connect phase is timed out
    // before establishing a connection, so don't need them for
    // TimeoutPhase::SECOND_CONNECT.
    Initialize(reads2, writes2, base::span<MockRead>(), base::span<MockWrite>(),
               SYNCHRONOUS);

    // Finish resolution.
    session_deps_.host_resolver->ResolveOnlyRequestNow();
    EXPECT_FALSE(test_delegate.has_result());
    EXPECT_EQ(LOAD_STATE_ESTABLISHING_PROXY_TUNNEL,
              connect_job->GetLoadState());

    // Wait until just before negotiation with the tunnel should time out.
    FastForwardBy(HttpProxyConnectJob::TunnelTimeoutForTesting() - kTinyTime);
    EXPECT_FALSE(test_delegate.has_result());

    if (timeout_phase == TimeoutPhase::SECOND_PROXY_HANDSHAKE) {
      FastForwardBy(kTinyTime);
      ASSERT_TRUE(test_delegate.has_result());
      EXPECT_THAT(test_delegate.WaitForResult(), test::IsError(ERR_TIMED_OUT));
      continue;
    }

    data_->Resume();
    ASSERT_TRUE(test_delegate.has_result());
    EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());
  }
}

TEST_P(HttpProxyConnectJobTest, ConnectionTimeoutMin) {
  // Set RTT estimate to a low value.
  base::TimeDelta rtt_estimate = base::TimeDelta::FromMilliseconds(1);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);

  EXPECT_LE(base::TimeDelta(), GetProxyConnectionTimeout());

  // Test against a large value.
  EXPECT_GE(base::TimeDelta::FromMinutes(10), GetProxyConnectionTimeout());

#if (defined(OS_ANDROID) || defined(OS_IOS))
  EXPECT_EQ(base::TimeDelta::FromSeconds(8), GetProxyConnectionTimeout());
#else
  EXPECT_EQ(base::TimeDelta::FromSeconds(30), GetProxyConnectionTimeout());
#endif
}

TEST_P(HttpProxyConnectJobTest, ConnectionTimeoutMax) {
  // Set RTT estimate to a high value.
  base::TimeDelta rtt_estimate = base::TimeDelta::FromSeconds(100);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);

  EXPECT_LE(base::TimeDelta(), GetProxyConnectionTimeout());

  // Test against a large value.
  EXPECT_GE(base::TimeDelta::FromMinutes(10), GetProxyConnectionTimeout());

#if (defined(OS_ANDROID) || defined(OS_IOS))
  EXPECT_EQ(base::TimeDelta::FromSeconds(30), GetProxyConnectionTimeout());
#else
  EXPECT_EQ(base::TimeDelta::FromSeconds(60), GetProxyConnectionTimeout());
#endif
}

// Tests the connection timeout values when the field trial parameters are
// specified.
TEST_P(HttpProxyConnectJobTest, ConnectionTimeoutWithExperiment) {
  // Timeout should be kMultiplier times the HTTP RTT estimate.
  const int kMultiplier = 4;
  const base::TimeDelta kMinTimeout = base::TimeDelta::FromSeconds(8);
  const base::TimeDelta kMaxTimeout = base::TimeDelta::FromSeconds(20);

  InitAdaptiveTimeoutFieldTrialWithParams(false, kMultiplier, kMultiplier,
                                          kMinTimeout, kMaxTimeout);
  EXPECT_LE(base::TimeDelta(), GetProxyConnectionTimeout());

  base::TimeDelta rtt_estimate = base::TimeDelta::FromSeconds(4);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  base::TimeDelta expected_connection_timeout = kMultiplier * rtt_estimate;
  EXPECT_EQ(expected_connection_timeout, GetProxyConnectionTimeout());

  // Connection timeout should not exceed kMaxTimeout.
  rtt_estimate = base::TimeDelta::FromSeconds(25);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMaxTimeout, GetProxyConnectionTimeout());

  // Connection timeout should not be less than kMinTimeout.
  rtt_estimate = base::TimeDelta::FromSeconds(0);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMinTimeout, GetProxyConnectionTimeout());
}

// Tests the connection timeout values when the field trial parameters are
// specified.
TEST_P(HttpProxyConnectJobTest, ConnectionTimeoutExperimentDifferentParams) {
  // Timeout should be kMultiplier times the HTTP RTT estimate.
  const int kMultiplier = 3;
  const base::TimeDelta kMinTimeout = base::TimeDelta::FromSeconds(2);
  const base::TimeDelta kMaxTimeout = base::TimeDelta::FromSeconds(30);

  InitAdaptiveTimeoutFieldTrialWithParams(false, kMultiplier, kMultiplier,
                                          kMinTimeout, kMaxTimeout);
  EXPECT_LE(base::TimeDelta(), GetProxyConnectionTimeout());

  base::TimeDelta rtt_estimate = base::TimeDelta::FromSeconds(2);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMultiplier * rtt_estimate, GetProxyConnectionTimeout());

  // A change in RTT estimate should also change the connection timeout.
  rtt_estimate = base::TimeDelta::FromSeconds(7);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMultiplier * rtt_estimate, GetProxyConnectionTimeout());

  // Connection timeout should not exceed kMaxTimeout.
  rtt_estimate = base::TimeDelta::FromSeconds(35);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMaxTimeout, GetProxyConnectionTimeout());

  // Connection timeout should not be less than kMinTimeout.
  rtt_estimate = base::TimeDelta::FromSeconds(0);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_EQ(kMinTimeout, GetProxyConnectionTimeout());
}

TEST_P(HttpProxyConnectJobTest, ConnectionTimeoutWithConnectionProperty) {
  const int kSecureMultiplier = 3;
  const int kNonSecureMultiplier = 5;
  const base::TimeDelta kMinTimeout = base::TimeDelta::FromSeconds(2);
  const base::TimeDelta kMaxTimeout = base::TimeDelta::FromSeconds(30);

  InitAdaptiveTimeoutFieldTrialWithParams(
      false, kSecureMultiplier, kNonSecureMultiplier, kMinTimeout, kMaxTimeout);

  const base::TimeDelta kRttEstimate = base::TimeDelta::FromSeconds(2);
  network_quality_estimator_->SetStartTimeNullHttpRtt(kRttEstimate);
  // By default, connection timeout should return the timeout for secure
  // proxies.
  if (GetParam() != HTTP) {
    EXPECT_EQ(kSecureMultiplier * kRttEstimate, GetProxyConnectionTimeout());
  } else {
    EXPECT_EQ(kNonSecureMultiplier * kRttEstimate, GetProxyConnectionTimeout());
  }
}

// Tests the connection timeout values when the field trial parameters are not
// specified.
TEST_P(HttpProxyConnectJobTest, ProxyPoolTimeoutWithExperimentDefaultParams) {
  InitAdaptiveTimeoutFieldTrialWithParams(true, 0, 0, base::TimeDelta(),
                                          base::TimeDelta());
  EXPECT_LE(base::TimeDelta(), GetProxyConnectionTimeout());

  // Timeout should be |http_rtt_multiplier| times the HTTP RTT
  // estimate.
  base::TimeDelta rtt_estimate = base::TimeDelta::FromMilliseconds(10);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  // Connection timeout should not be less than the HTTP RTT estimate.
  EXPECT_LE(rtt_estimate, GetProxyConnectionTimeout());

  // A change in RTT estimate should also change the connection timeout.
  rtt_estimate = base::TimeDelta::FromSeconds(10);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  // Connection timeout should not be less than the HTTP RTT estimate.
  EXPECT_LE(rtt_estimate, GetProxyConnectionTimeout());

  // Set RTT to a very large value.
  rtt_estimate = base::TimeDelta::FromMinutes(60);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_GT(rtt_estimate, GetProxyConnectionTimeout());

  // Set RTT to a very small value.
  rtt_estimate = base::TimeDelta::FromSeconds(0);
  network_quality_estimator_->SetStartTimeNullHttpRtt(rtt_estimate);
  EXPECT_LT(rtt_estimate, GetProxyConnectionTimeout());
}

}  // namespace net

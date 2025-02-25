// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "content/browser/devtools/protocol/fetch_handler.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/browser/devtools/devtools_agent_host_impl.h"
#include "content/browser/devtools/devtools_io_context.h"
#include "content/browser/devtools/devtools_stream_pipe.h"
#include "content/browser/devtools/devtools_url_loader_interceptor.h"
#include "content/browser/devtools/protocol/network_handler.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/features.h"

namespace content {
namespace protocol {

// static
std::vector<FetchHandler*> FetchHandler::ForAgentHost(
    DevToolsAgentHostImpl* host) {
  return host->HandlersByName<FetchHandler>(Fetch::Metainfo::domainName);
}

FetchHandler::FetchHandler(
    DevToolsIOContext* io_context,
    base::RepeatingClosure update_loader_factories_callback)
    : DevToolsDomainHandler(Fetch::Metainfo::domainName),
      io_context_(io_context),
      update_loader_factories_callback_(
          std::move(update_loader_factories_callback)),
      weak_factory_(this) {}

FetchHandler::~FetchHandler() = default;

void FetchHandler::Wire(UberDispatcher* dispatcher) {
  frontend_.reset(new Fetch::Frontend(dispatcher->channel()));
  Fetch::Dispatcher::wire(dispatcher, this);
}

DevToolsNetworkInterceptor::InterceptionStage RequestStageToInterceptorStage(
    const Fetch::RequestStage& stage) {
  if (stage == Fetch::RequestStageEnum::Request)
    return DevToolsNetworkInterceptor::REQUEST;
  if (stage == Fetch::RequestStageEnum::Response)
    return DevToolsNetworkInterceptor::RESPONSE;
  NOTREACHED();
  return DevToolsNetworkInterceptor::REQUEST;
}

Response ToInterceptionPatterns(
    const Maybe<Array<Fetch::RequestPattern>>& maybe_patterns,
    std::vector<DevToolsNetworkInterceptor::Pattern>* result) {
  result->clear();
  if (!maybe_patterns.isJust()) {
    result->push_back(DevToolsNetworkInterceptor::Pattern(
        "*", {}, DevToolsNetworkInterceptor::REQUEST));
    return Response::OK();
  }
  Array<Fetch::RequestPattern>& patterns = *maybe_patterns.fromJust();
  for (size_t i = 0; i < patterns.length(); ++i) {
    base::flat_set<ResourceType> resource_types;
    std::string resource_type = patterns.get(i)->GetResourceType("");
    if (!resource_type.empty()) {
      if (!NetworkHandler::AddInterceptedResourceType(resource_type,
                                                      &resource_types)) {
        return Response::InvalidParams(
            base::StringPrintf("Unknown resource type in fetch filter: '%s'",
                               resource_type.c_str()));
      }
    }
    result->push_back(DevToolsNetworkInterceptor::Pattern(
        patterns.get(i)->GetUrlPattern("*"), std::move(resource_types),
        RequestStageToInterceptorStage(patterns.get(i)->GetRequestStage(
            Fetch::RequestStageEnum::Request))));
  }
  return Response::OK();
}

bool FetchHandler::MaybeCreateProxyForInterception(
    RenderProcessHost* rph,
    const base::UnguessableToken& frame_token,
    bool is_navigation,
    bool is_download,
    network::mojom::URLLoaderFactoryRequest* target_factory_request) {
  return interceptor_ && interceptor_->CreateProxyForInterception(
                             rph, frame_token, is_navigation, is_download,
                             target_factory_request);
}

Response FetchHandler::Enable(Maybe<Array<Fetch::RequestPattern>> patterns,
                              Maybe<bool> handleAuth) {
  if (!interceptor_) {
    if (!base::FeatureList::IsEnabled(network::features::kNetworkService)) {
      return Response::Error(
          "Fetch domain is only supported with "
          "--enable-features=NetworkService");
    }
    interceptor_ =
        std::make_unique<DevToolsURLLoaderInterceptor>(base::BindRepeating(
            &FetchHandler::RequestIntercepted, weak_factory_.GetWeakPtr()));
  }
  std::vector<DevToolsNetworkInterceptor::Pattern> interception_patterns;
  Response response = ToInterceptionPatterns(patterns, &interception_patterns);
  if (!response.isSuccess())
    return response;
  if (!interception_patterns.size() && handleAuth.fromMaybe(false))
    return Response::InvalidParams(
        "Can\'t specify empty patterns with handleAuth set");
  interceptor_->SetPatterns(std::move(interception_patterns),
                            handleAuth.fromMaybe(false));
  update_loader_factories_callback_.Run();
  return Response::OK();
}

Response FetchHandler::Disable() {
  const bool was_enabled = !!interceptor_;
  interceptor_.reset();
  if (was_enabled)
    update_loader_factories_callback_.Run();
  return Response::OK();
}

namespace {
using ContinueInterceptedRequestCallback =
    DevToolsNetworkInterceptor::ContinueInterceptedRequestCallback;

template <typename Callback, typename Base, typename... Args>
class CallbackWrapper : public Base {
 public:
  explicit CallbackWrapper(std::unique_ptr<Callback> callback)
      : callback_(std::move(callback)) {}

 private:
  friend typename std::unique_ptr<CallbackWrapper>::deleter_type;

  void sendSuccess(Args... args) override {
    callback_->sendSuccess(std::forward<Args>(args)...);
  }
  void sendFailure(const DispatchResponse& response) override {
    callback_->sendFailure(response);
  }
  void fallThrough() override { NOTREACHED(); }
  ~CallbackWrapper() override {}

  std::unique_ptr<Callback> callback_;
};

template <typename Callback>
std::unique_ptr<CallbackWrapper<Callback, ContinueInterceptedRequestCallback>>
WrapCallback(std::unique_ptr<Callback> cb) {
  return std::make_unique<
      CallbackWrapper<Callback, ContinueInterceptedRequestCallback>>(
      std::move(cb));
}

template <typename Callback>
bool ValidateHeaders(Fetch::HeaderEntry* entry, Callback* callback) {
  if (!net::HttpUtil::IsValidHeaderName(entry->GetName()) ||
      !net::HttpUtil::IsValidHeaderValue(entry->GetValue())) {
    callback->sendFailure(Response::InvalidParams("Invalid header"));
    return false;
  }
  return true;
}
}  // namespace

void FetchHandler::FailRequest(const String& requestId,
                               const String& errorReason,
                               std::unique_ptr<FailRequestCallback> callback) {
  if (!interceptor_) {
    callback->sendFailure(Response::Error("Fetch domain is not enabled"));
    return;
  }
  bool ok = false;
  net::Error reason = NetworkHandler::NetErrorFromString(errorReason, &ok);
  if (!ok) {
    callback->sendFailure(Response::InvalidParams("Unknown errorReason"));
    return;
  }
  auto modifications =
      std::make_unique<DevToolsNetworkInterceptor::Modifications>(reason);
  interceptor_->ContinueInterceptedRequest(requestId, std::move(modifications),
                                           WrapCallback(std::move(callback)));
}

void FetchHandler::FulfillRequest(
    const String& requestId,
    int responseCode,
    std::unique_ptr<Array<Fetch::HeaderEntry>> responseHeaders,
    Maybe<Binary> body,
    Maybe<String> responsePhrase,
    std::unique_ptr<FulfillRequestCallback> callback) {
  if (!interceptor_) {
    callback->sendFailure(Response::Error("Fetch domain is not enabled"));
    return;
  }
  std::string status_phrase =
      responsePhrase.isJust()
          ? responsePhrase.fromJust()
          : net::GetHttpReasonPhrase(
                static_cast<net::HttpStatusCode>(responseCode));
  if (status_phrase.empty()) {
    callback->sendFailure(
        Response::Error("Invalid http status code or phrase"));
    return;
  }
  std::string headers =
      base::StringPrintf("HTTP/1.1 %d %s", responseCode, status_phrase.c_str());
  headers.append(1, '\0');
  if (responseHeaders) {
    for (size_t i = 0; i < responseHeaders->length(); ++i) {
      auto* entry = responseHeaders->get(i);
      if (!ValidateHeaders(entry, callback.get()))
        return;
      headers.append(entry->GetName());
      headers.append(":");
      headers.append(entry->GetValue());
      headers.append(1, '\0');
    }
  }
  headers.append(1, '\0');
  auto modifications =
      std::make_unique<DevToolsNetworkInterceptor::Modifications>(
          base::MakeRefCounted<net::HttpResponseHeaders>(headers),
          body.isJust() ? body.fromJust().bytes() : nullptr);
  interceptor_->ContinueInterceptedRequest(requestId, std::move(modifications),
                                           WrapCallback(std::move(callback)));
}

void FetchHandler::ContinueRequest(
    const String& requestId,
    Maybe<String> url,
    Maybe<String> method,
    Maybe<String> postData,
    Maybe<Array<Fetch::HeaderEntry>> headers,
    std::unique_ptr<ContinueRequestCallback> callback) {
  if (!interceptor_) {
    callback->sendFailure(Response::Error("Fetch domain is not enabled"));
    return;
  }
  std::unique_ptr<DevToolsNetworkInterceptor::Modifications::HeadersVector>
      request_headers;
  if (headers.isJust()) {
    request_headers = std::make_unique<
        DevToolsNetworkInterceptor::Modifications::HeadersVector>();
    for (size_t i = 0; i < headers.fromJust()->length(); ++i) {
      auto* entry = headers.fromJust()->get(i);
      if (!ValidateHeaders(entry, callback.get()))
        return;
      request_headers->emplace_back(entry->GetName(), entry->GetValue());
    }
  }
  auto modifications =
      std::make_unique<DevToolsNetworkInterceptor::Modifications>(
          std::move(url), std::move(method), std::move(postData),
          std::move(request_headers));
  interceptor_->ContinueInterceptedRequest(requestId, std::move(modifications),
                                           WrapCallback(std::move(callback)));
}

void FetchHandler::ContinueWithAuth(
    const String& requestId,
    std::unique_ptr<protocol::Fetch::AuthChallengeResponse>
        authChallengeResponse,
    std::unique_ptr<ContinueWithAuthCallback> callback) {
  if (!interceptor_) {
    callback->sendFailure(Response::Error("Fetch domain is not enabled"));
    return;
  }
  using AuthChallengeResponse =
      DevToolsNetworkInterceptor::AuthChallengeResponse;
  std::unique_ptr<AuthChallengeResponse> auth_response;
  std::string type = authChallengeResponse->GetResponse();
  if (type == Network::AuthChallengeResponse::ResponseEnum::Default) {
    auth_response = std::make_unique<AuthChallengeResponse>(
        AuthChallengeResponse::kDefault);
  } else if (type == Network::AuthChallengeResponse::ResponseEnum::CancelAuth) {
    auth_response = std::make_unique<AuthChallengeResponse>(
        AuthChallengeResponse::kCancelAuth);
  } else if (type ==
             Network::AuthChallengeResponse::ResponseEnum::ProvideCredentials) {
    auth_response = std::make_unique<AuthChallengeResponse>(
        base::UTF8ToUTF16(authChallengeResponse->GetUsername("")),
        base::UTF8ToUTF16(authChallengeResponse->GetPassword("")));
  } else {
    callback->sendFailure(
        Response::InvalidParams("Unrecognized authChallengeResponse"));
    return;
  }
  auto modifications =
      std::make_unique<DevToolsNetworkInterceptor::Modifications>(
          std::move(auth_response));
  interceptor_->ContinueInterceptedRequest(requestId, std::move(modifications),
                                           WrapCallback(std::move(callback)));
}

void FetchHandler::GetResponseBody(
    const String& requestId,
    std::unique_ptr<GetResponseBodyCallback> callback) {
  auto weapped_callback = std::make_unique<CallbackWrapper<
      GetResponseBodyCallback,
      DevToolsNetworkInterceptor::GetResponseBodyForInterceptionCallback,
      const std::string&, bool>>(std::move(callback));
  interceptor_->GetResponseBody(requestId, std::move(weapped_callback));
}

void FetchHandler::TakeResponseBodyAsStream(
    const String& requestId,
    std::unique_ptr<TakeResponseBodyAsStreamCallback> callback) {
  interceptor_->TakeResponseBodyPipe(
      requestId,
      base::BindOnce(&FetchHandler::OnResponseBodyPipeTaken,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void FetchHandler::OnResponseBodyPipeTaken(
    std::unique_ptr<TakeResponseBodyAsStreamCallback> callback,
    Response response,
    mojo::ScopedDataPipeConsumerHandle pipe,
    const std::string& mime_type) {
  DCHECK_EQ(response.isSuccess(), pipe.is_valid());
  if (!response.isSuccess()) {
    callback->sendFailure(std::move(response));
    return;
  }
  // The pipe stream is owned only by io_context after we return.
  bool is_binary = !DevToolsIOContext::IsTextMimeType(mime_type);
  auto stream =
      DevToolsStreamPipe::Create(io_context_, std::move(pipe), is_binary);
  callback->sendSuccess(stream->handle());
}

namespace {

std::unique_ptr<Array<Fetch::HeaderEntry>> ToHeaderEntryArray(
    scoped_refptr<net::HttpResponseHeaders> headers) {
  auto result = std::make_unique<Array<Fetch::HeaderEntry>>();
  size_t iterator = 0;
  std::string name;
  std::string value;
  while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
    result->addItem(
        Fetch::HeaderEntry::Create().SetName(name).SetValue(value).Build());
  }
  return result;
}

}  // namespace

void FetchHandler::RequestIntercepted(
    std::unique_ptr<InterceptedRequestInfo> info) {
  protocol::Maybe<protocol::Network::ErrorReason> error_reason;
  if (info->response_error_code < 0)
    error_reason = NetworkHandler::NetErrorToString(info->response_error_code);

  Maybe<int> status_code;
  Maybe<Array<Fetch::HeaderEntry>> response_headers;
  if (info->response_headers) {
    status_code = info->response_headers->response_code();
    response_headers = ToHeaderEntryArray(info->response_headers);
  }

  if (info->auth_challenge) {
    auto auth_challenge =
        Fetch::AuthChallenge::Create()
            .SetSource(info->auth_challenge->is_proxy
                           ? Network::AuthChallenge::SourceEnum::Proxy
                           : Network::AuthChallenge::SourceEnum::Server)
            .SetOrigin(info->auth_challenge->challenger.Serialize())
            .SetScheme(info->auth_challenge->scheme)
            .SetRealm(info->auth_challenge->realm)
            .Build();
    frontend_->AuthRequired(
        info->interception_id, std::move(info->network_request),
        info->frame_id.ToString(),
        NetworkHandler::ResourceTypeToString(info->resource_type),
        std::move(auth_challenge));
    return;
  }
  frontend_->RequestPaused(
      info->interception_id, std::move(info->network_request),
      info->frame_id.ToString(),
      NetworkHandler::ResourceTypeToString(info->resource_type),
      std::move(error_reason), std::move(status_code),
      std::move(response_headers));
}

}  // namespace protocol
}  // namespace content

// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REMOTING_SIGNALING_FTL_GRPC_CONTEXT_H_
#define REMOTING_SIGNALING_FTL_GRPC_CONTEXT_H_

#include <memory>
#include <string>

#include "base/macros.h"
#include "remoting/signaling/ftl.pb.h"
#include "remoting/signaling/grpc_support/grpc_channel.h"

namespace grpc {
class ClientContext;
}  // namespace grpc

namespace remoting {

// This is the class for creating context objects to be used when connecting
// to FTL backend.
class FtlGrpcContext final {
 public:
  static std::string GetChromotingAppIdentifier();
  static ftl::Id CreateIdFromString(const std::string& ftl_id);
  static GrpcChannelSharedPtr CreateChannel();
  static std::unique_ptr<grpc::ClientContext> CreateClientContext();
  static ftl::RequestHeader CreateRequestHeader(
      const std::string& ftl_auth_token = {});

  static void SetChannelForTesting(GrpcChannelSharedPtr channel);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(FtlGrpcContext);
};

}  // namespace remoting

#endif  // REMOTING_SIGNALING_FTL_GRPC_CONTEXT_H_

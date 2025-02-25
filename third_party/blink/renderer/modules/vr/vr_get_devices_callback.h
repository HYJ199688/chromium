// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_VR_VR_GET_DEVICES_CALLBACK_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_VR_VR_GET_DEVICES_CALLBACK_H_

#include "device/vr/public/mojom/vr_service.mojom-blink.h"
#include "third_party/blink/renderer/modules/vr/vr_display.h"
#include "third_party/blink/renderer/platform/heap/handle.h"

namespace blink {

class ScriptPromiseResolver;

// Success and failure callbacks for getDisplays.
class VRGetDevicesCallback final {
  USING_FAST_MALLOC(VRGetDevicesCallback);

 public:
  VRGetDevicesCallback(ScriptPromiseResolver*);
  ~VRGetDevicesCallback();

  void OnSuccess(VRDisplayVector);
  void OnError();

 private:
  Persistent<ScriptPromiseResolver> resolver_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_VR_VR_GET_DEVICES_CALLBACK_H_

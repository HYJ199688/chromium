// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/picture_in_picture/picture_in_picture_controller_impl.h"

#include <limits>
#include <utility>

#include "base/bind_helpers.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/common/manifest/web_display_mode.h"
#include "third_party/blink/public/mojom/feature_policy/feature_policy.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/core/origin_trials/origin_trials.h"
#include "third_party/blink/renderer/modules/picture_in_picture/enter_picture_in_picture_event.h"
#include "third_party/blink/renderer/modules/picture_in_picture/picture_in_picture_window.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

namespace {

bool ShouldShowPlayPauseButton(const HTMLVideoElement& element) {
  return element.GetLoadType() != WebMediaPlayer::kLoadTypeMediaStream &&
         element.duration() != std::numeric_limits<double>::infinity();
}

}  // namespace

// static
PictureInPictureControllerImpl* PictureInPictureControllerImpl::Create(
    Document& document) {
  return MakeGarbageCollected<PictureInPictureControllerImpl>(document);
}

// static
PictureInPictureControllerImpl& PictureInPictureControllerImpl::From(
    Document& document) {
  return static_cast<PictureInPictureControllerImpl&>(
      PictureInPictureController::From(document));
}

bool PictureInPictureControllerImpl::PictureInPictureEnabled() const {
  return IsDocumentAllowed() == Status::kEnabled;
}

PictureInPictureController::Status
PictureInPictureControllerImpl::IsDocumentAllowed() const {
  DCHECK(GetSupplementable());

  // If document has been detached from a frame, return kFrameDetached status.
  LocalFrame* frame = GetSupplementable()->GetFrame();
  if (!frame)
    return Status::kFrameDetached;

  // `GetPictureInPictureEnabled()` returns false when the embedder or the
  // system forbids the page from using Picture-in-Picture.
  DCHECK(GetSupplementable()->GetSettings());
  if (!GetSupplementable()->GetSettings()->GetPictureInPictureEnabled())
    return Status::kDisabledBySystem;

  // If document is not allowed to use the policy-controlled feature named
  // "picture-in-picture", return kDisabledByFeaturePolicy status.
  if (RuntimeEnabledFeatures::PictureInPictureAPIEnabled() &&
      !GetSupplementable()->IsFeatureEnabled(
          blink::mojom::FeaturePolicyFeature::kPictureInPicture,
          ReportOptions::kReportOnFailure)) {
    return Status::kDisabledByFeaturePolicy;
  }

  return Status::kEnabled;
}

PictureInPictureController::Status
PictureInPictureControllerImpl::IsElementAllowed(
    const HTMLVideoElement& element) const {
  PictureInPictureController::Status status = IsDocumentAllowed();
  if (status != Status::kEnabled)
    return status;

  if (element.getReadyState() == HTMLMediaElement::kHaveNothing)
    return Status::kMetadataNotLoaded;

  if (!element.HasVideo())
    return Status::kVideoTrackNotAvailable;

  if (element.FastHasAttribute(html_names::kDisablepictureinpictureAttr))
    return Status::kDisabledByAttribute;

  return Status::kEnabled;
}

void PictureInPictureControllerImpl::EnterPictureInPicture(
    HTMLVideoElement* element,
    ScriptPromiseResolver* resolver) {
  DCHECK(element->GetWebMediaPlayer());

  if (picture_in_picture_element_ == element) {
    if (resolver)
      resolver->Resolve(picture_in_picture_window_);

    return;
  }

  if (!EnsureService())
    return;

  if (element->DisplayType() == WebMediaPlayer::DisplayType::kFullscreen)
    Fullscreen::ExitFullscreen(*GetSupplementable());

  element->GetWebMediaPlayer()->OnRequestPictureInPicture();

  picture_in_picture_service_->StartSession(
      element->GetWebMediaPlayer()->GetDelegateId(),
      element->GetWebMediaPlayer()->GetSurfaceId(),
      element->GetWebMediaPlayer()->NaturalSize(),
      ShouldShowPlayPauseButton(*element), ShouldShowMuteButton(*element),
      WTF::Bind(&PictureInPictureControllerImpl::OnEnteredPictureInPicture,
                WrapPersistent(this), WrapPersistent(element),
                WrapPersistent(resolver)));
}

void PictureInPictureControllerImpl::OnEnteredPictureInPicture(
    HTMLVideoElement* element,
    ScriptPromiseResolver* resolver,
    const WebSize& picture_in_picture_window_size) {
  if (IsElementAllowed(*element) != Status::kEnabled) {
    if (resolver) {
      resolver->Reject(
          DOMException::Create(DOMExceptionCode::kInvalidStateError, ""));
    }
    ExitPictureInPicture(element, nullptr);
    return;
  }

  picture_in_picture_element_ = element;

  picture_in_picture_element_->OnEnteredPictureInPicture();

  // Closes the current Picture-in-Picture window if any.
  if (picture_in_picture_window_)
    picture_in_picture_window_->OnClose();

  picture_in_picture_window_ = MakeGarbageCollected<PictureInPictureWindow>(
      GetSupplementable(), picture_in_picture_window_size);

  picture_in_picture_element_->DispatchEvent(
      *EnterPictureInPictureEvent::Create(
          event_type_names::kEnterpictureinpicture,
          WrapPersistent(picture_in_picture_window_.Get())));

  if (!EnsureService())
    return;

  if (delegate_binding_.is_bound())
    delegate_binding_.Close();

  mojom::blink::PictureInPictureDelegatePtr delegate;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      picture_in_picture_element_->GetDocument().GetTaskRunner(
          TaskType::kMediaElementEvent);
  delegate_binding_.Bind(mojo::MakeRequest(&delegate, task_runner),
                         task_runner);
  picture_in_picture_service_->SetDelegate(std::move(delegate));

  if (resolver)
    resolver->Resolve(picture_in_picture_window_);
}

void PictureInPictureControllerImpl::ExitPictureInPicture(
    HTMLVideoElement* element,
    ScriptPromiseResolver* resolver) {
  if (!EnsureService())
    return;

  picture_in_picture_service_->EndSession(
      WTF::Bind(&PictureInPictureControllerImpl::OnExitedPictureInPicture,
                WrapPersistent(this), WrapPersistent(resolver)));
  delegate_binding_.Close();
}

void PictureInPictureControllerImpl::OnExitedPictureInPicture(
    ScriptPromiseResolver* resolver) {
  DCHECK(GetSupplementable());

  // Bail out if document is not active.
  if (!GetSupplementable()->IsActive())
    return;

  if (picture_in_picture_window_)
    picture_in_picture_window_->OnClose();

  if (picture_in_picture_element_) {
    HTMLVideoElement* element = picture_in_picture_element_;
    picture_in_picture_element_ = nullptr;

    element->OnExitedPictureInPicture();
    element->DispatchEvent(
        *Event::CreateBubble(event_type_names::kLeavepictureinpicture));
  }

  if (resolver)
    resolver->Resolve();
}

Element* PictureInPictureControllerImpl::PictureInPictureElement() const {
  return picture_in_picture_element_;
}

Element* PictureInPictureControllerImpl::PictureInPictureElement(
    TreeScope& scope) const {
  if (!picture_in_picture_element_)
    return nullptr;

  return scope.AdjustedElement(*picture_in_picture_element_);
}

bool PictureInPictureControllerImpl::IsPictureInPictureElement(
    const Element* element) const {
  DCHECK(element);
  return element == picture_in_picture_element_;
}

bool PictureInPictureControllerImpl::IsPictureInPictureShadowHost(
    const Element& host) const {
  if (!picture_in_picture_element_)
    return false;

  return host.GetTreeScope().AdjustedElement(*picture_in_picture_element_) ==
         &host;
}

void PictureInPictureControllerImpl::AddToAutoPictureInPictureElementsList(
    HTMLVideoElement* element) {
  RemoveFromAutoPictureInPictureElementsList(element);
  auto_picture_in_picture_elements_.push_back(element);
}

void PictureInPictureControllerImpl::RemoveFromAutoPictureInPictureElementsList(
    HTMLVideoElement* element) {
  DCHECK(element);
  auto it = std::find(auto_picture_in_picture_elements_.begin(),
                      auto_picture_in_picture_elements_.end(), element);
  if (it != auto_picture_in_picture_elements_.end())
    auto_picture_in_picture_elements_.erase(it);
}

HTMLVideoElement* PictureInPictureControllerImpl::AutoPictureInPictureElement()
    const {
  return auto_picture_in_picture_elements_.IsEmpty()
             ? nullptr
             : auto_picture_in_picture_elements_.back();
}

bool PictureInPictureControllerImpl::IsEnterAutoPictureInPictureAllowed()
    const {
  // Entering Auto Picture-in-Picture is allowed if one of these conditions is
  // true:
  // - Document runs in a Chrome Extension.
  // - Document is in fullscreen.
  // - Document is in a PWA window that runs in the scope of the PWA.
  if (!(GetSupplementable()->Url().ProtocolIs("chrome-extension") ||
        Fullscreen::FullscreenElementFrom(*GetSupplementable()) ||
        (GetSupplementable()->View() &&
         GetSupplementable()->View()->DisplayMode() !=
             WebDisplayMode::kWebDisplayModeBrowser &&
         GetSupplementable()->IsInWebAppScope()))) {
    return false;
  }

  // Don't allow if there's already an element in Auto Picture-in-Picture.
  if (picture_in_picture_element_)
    return false;

  // Don't allow if there's no element eligible to enter Auto Picture-in-Picture
  if (!AutoPictureInPictureElement())
    return false;

  // Don't allow if video won't resume playing automatically when it becomes
  // visible again.
  if (AutoPictureInPictureElement()->PausedWhenVisible())
    return false;

  // Allow if video is allowed to enter Picture-in-Picture.
  return (IsElementAllowed(*AutoPictureInPictureElement()) == Status::kEnabled);
}

bool PictureInPictureControllerImpl::IsExitAutoPictureInPictureAllowed() const {
  // Don't allow exiting Auto Picture-in-Picture if there's no eligible element
  // to exit Auto Picture-in-Picture.
  if (!AutoPictureInPictureElement())
    return false;

  // Allow if the element already in Picture-in-Picture is the same as the one
  // eligible to exit Auto Picture-in-Picture.
  return (picture_in_picture_element_ == AutoPictureInPictureElement());
}

void PictureInPictureControllerImpl::PageVisibilityChanged() {
  DCHECK(GetSupplementable());

  // If page becomes visible and exiting Auto Picture-in-Picture is allowed,
  // exit Picture-in-Picture.
  if (GetSupplementable()->IsPageVisible() &&
      IsExitAutoPictureInPictureAllowed()) {
    ExitPictureInPicture(picture_in_picture_element_, nullptr);
    return;
  }

  // If page becomes hidden and entering Auto Picture-in-Picture is allowed,
  // enter Picture-in-Picture.
  if (GetSupplementable()->hidden() && IsEnterAutoPictureInPictureAllowed()) {
    EnterPictureInPicture(AutoPictureInPictureElement(), nullptr);
  }
}

void PictureInPictureControllerImpl::ContextDestroyed(Document*) {
  picture_in_picture_service_.reset();
  delegate_binding_.Close();
}

void PictureInPictureControllerImpl::OnPictureInPictureStateChange() {
  DCHECK(picture_in_picture_element_);
  DCHECK(picture_in_picture_element_->GetWebMediaPlayer());

  picture_in_picture_service_->UpdateSession(
      picture_in_picture_element_->GetWebMediaPlayer()->GetDelegateId(),
      picture_in_picture_element_->GetWebMediaPlayer()->GetSurfaceId(),
      picture_in_picture_element_->GetWebMediaPlayer()->NaturalSize(),
      ShouldShowPlayPauseButton(*picture_in_picture_element_),
      ShouldShowMuteButton(*picture_in_picture_element_));
}

void PictureInPictureControllerImpl::PictureInPictureWindowSizeChanged(
    const blink::WebSize& size) {
  if (picture_in_picture_window_)
    picture_in_picture_window_->OnResize(size);
}

bool PictureInPictureControllerImpl::ShouldShowMuteButton(
    const HTMLVideoElement& element) {
  DCHECK(GetSupplementable());
  return element.HasAudio() && origin_trials::MuteButtonEnabled(
                                   GetSupplementable()->GetExecutionContext());
}

void PictureInPictureControllerImpl::Trace(blink::Visitor* visitor) {
  visitor->Trace(picture_in_picture_element_);
  visitor->Trace(auto_picture_in_picture_elements_);
  visitor->Trace(picture_in_picture_window_);
  PictureInPictureController::Trace(visitor);
  PageVisibilityObserver::Trace(visitor);
  DocumentShutdownObserver::Trace(visitor);
}

PictureInPictureControllerImpl::PictureInPictureControllerImpl(
    Document& document)
    : PictureInPictureController(document),
      PageVisibilityObserver(document.GetPage()),
      delegate_binding_(this) {}

bool PictureInPictureControllerImpl::EnsureService() {
  if (picture_in_picture_service_)
    return true;

  if (!GetSupplementable()->GetFrame())
    return false;

  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      GetSupplementable()->GetFrame()->GetTaskRunner(
          TaskType::kMediaElementEvent);
  GetSupplementable()->GetFrame()->GetInterfaceProvider().GetInterface(
      mojo::MakeRequest(&picture_in_picture_service_, task_runner));
  return true;
}

}  // namespace blink

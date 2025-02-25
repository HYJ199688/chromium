// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/skia_output_device_gl.h"

#include <utility>

#include "gpu/command_buffer/common/swap_buffers_complete_params.h"
#include "gpu/command_buffer/service/feature_info.h"
#include "gpu/command_buffer/service/gl_utils.h"
#include "gpu/ipc/service/image_transport_surface.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkSurfaceProps.h"
#include "third_party/skia/include/gpu/GrBackendSurface.h"
#include "third_party/skia/include/gpu/GrContext.h"
#include "third_party/skia/include/gpu/gl/GrGLTypes.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_version_info.h"

namespace viz {

SkiaOutputDeviceGL::SkiaOutputDeviceGL(
    gpu::SurfaceHandle surface_handle,
    scoped_refptr<gpu::gles2::FeatureInfo> feature_info,
    const DidSwapBufferCompleteCallback& did_swap_buffer_complete_callback)
    : SkiaOutputDevice(did_swap_buffer_complete_callback),
      surface_handle_(surface_handle),
      feature_info_(feature_info) {
  DCHECK(surface_handle_);
  gl_surface_ = gpu::ImageTransportSurface::CreateNativeSurface(
      nullptr, surface_handle_, gl::GLSurfaceFormat());
}

void SkiaOutputDeviceGL::Initialize(GrContext* gr_context,
                                    gl::GLContext* gl_context) {
  DCHECK(gr_context);
  DCHECK(gl_context);
  gr_context_ = gr_context;

  gl::CurrentGL* current_gl = gl_context->GetCurrentGL();
  DCHECK(current_gl);

  // Get alpha and stencil bits from the default frame buffer.
  glBindFramebufferEXT(GL_FRAMEBUFFER, 0);
  gr_context_->resetContext(kRenderTarget_GrGLBackendState);
  const auto* version = current_gl->Version;
  GLint stencil_bits = 0;
  GLint alpha_bits = 0;
  if (version->is_desktop_core_profile) {
    glGetFramebufferAttachmentParameterivEXT(
        GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE,
        &stencil_bits);
    glGetFramebufferAttachmentParameterivEXT(
        GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE,
        &alpha_bits);
  } else {
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    glGetIntegerv(GL_ALPHA_BITS, &alpha_bits);
  }
  CHECK_GL_ERROR();
  supports_stencil_ = stencil_bits > 0;
  supports_alpha_ = alpha_bits > 0;

  supports_post_sub_buffer_ = gl_surface_->SupportsPostSubBuffer();
  if (feature_info_->workarounds()
          .disable_post_sub_buffers_for_onscreen_surfaces)
    supports_post_sub_buffer_ = false;
}

SkiaOutputDeviceGL::~SkiaOutputDeviceGL() {}

scoped_refptr<gl::GLSurface> SkiaOutputDeviceGL::gl_surface() {
  return gl_surface_;
}

void SkiaOutputDeviceGL::Reshape(const gfx::Size& size,
                                 float device_scale_factor,
                                 const gfx::ColorSpace& color_space,
                                 bool has_alpha) {
  // Conversion to GLSurface's color space follows the same logic as in
  // gl::GetGLColorSpace().
  gl::GLSurface::ColorSpace surface_color_space =
      color_space.IsHDR() ? gl::GLSurface::ColorSpace::SCRGB_LINEAR
                          : gl::GLSurface::ColorSpace::UNSPECIFIED;
  if (!gl_surface_->Resize(size, device_scale_factor, surface_color_space,
                           has_alpha)) {
    LOG(FATAL) << "Failed to resize.";
    // TODO(penghuang): Handle the failure.
  }
  SkSurfaceProps surface_props =
      SkSurfaceProps(0, SkSurfaceProps::kLegacyFontHost_InitType);

  GrGLFramebufferInfo framebuffer_info;
  framebuffer_info.fFBOID = gl_surface_->GetBackingFramebufferObject();
  framebuffer_info.fFormat = supports_alpha_ ? GL_RGBA8 : GL_RGB8_OES;
  GrBackendRenderTarget render_target(size.width(), size.height(), 0, 8,
                                      framebuffer_info);
  auto origin = gl_surface_->FlipsVertically() ? kTopLeft_GrSurfaceOrigin
                                               : kBottomLeft_GrSurfaceOrigin;
  auto color_type =
      supports_alpha_ ? kRGBA_8888_SkColorType : kRGB_888x_SkColorType;
  draw_surface_ = SkSurface::MakeFromBackendRenderTarget(
      gr_context_, render_target, origin, color_type,
      color_space.ToSkColorSpace(), &surface_props);
  DCHECK(draw_surface_);
}

gfx::SwapResponse SkiaOutputDeviceGL::SwapBuffers(
    BufferPresentedCallback feedback) {
  // TODO(backer): Support SwapBuffersAsync
  StartSwapBuffers({});
  return FinishSwapBuffers(gl_surface_->SwapBuffers(std::move(feedback)));
}

bool SkiaOutputDeviceGL::SupportPostSubBuffer() {
  return supports_post_sub_buffer_;
}

gfx::SwapResponse SkiaOutputDeviceGL::PostSubBuffer(
    const gfx::Rect& rect,
    BufferPresentedCallback feedback) {
  // TODO(backer): Support PostSubBufferAsync
  StartSwapBuffers({});
  return FinishSwapBuffers(gl_surface_->PostSubBuffer(
      rect.x(), rect.y(), rect.width(), rect.height(), std::move(feedback)));
}

}  // namespace viz

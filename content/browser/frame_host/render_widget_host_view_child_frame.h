// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_CHILD_FRAME_H_
#define CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_CHILD_FRAME_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <vector>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "build/build_config.h"
#include "cc/resources/returned_resource.h"
#include "cc/surfaces/surface_factory_client.h"
#include "cc/surfaces/surface_id_allocator.h"
#include "content/browser/compositor/image_transport_factory.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/common/content_export.h"
#include "content/public/browser/readback_types.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_widget_types.h"

namespace cc {
class SurfaceFactory;
enum class SurfaceDrawStatus;
}

struct ViewHostMsg_TextInputState_Params;

namespace content {
class CrossProcessFrameConnector;
class RenderWidgetHost;
class RenderWidgetHostImpl;
class RenderWidgetHostViewChildFrameTest;
class RenderWidgetHostViewGuestSurfaceTest;

// RenderWidgetHostViewChildFrame implements the view for a RenderWidgetHost
// associated with content being rendered in a separate process from
// content that is embedding it. This is not a platform-specific class; rather,
// the embedding renderer process implements the platform containing the
// widget, and the top-level frame's RenderWidgetHostView will ultimately
// manage all native widget interaction.
//
// See comments in render_widget_host_view.h about this class and its members.
class CONTENT_EXPORT RenderWidgetHostViewChildFrame
    : public RenderWidgetHostViewBase,
      public cc::SurfaceFactoryClient {
 public:
  explicit RenderWidgetHostViewChildFrame(RenderWidgetHost* widget);
  ~RenderWidgetHostViewChildFrame() override;

  void set_cross_process_frame_connector(
      CrossProcessFrameConnector* frame_connector) {
    frame_connector_ = frame_connector;
  }

  // This functions registers single-use callbacks that want to be notified when
  // the next frame is swapped. The callback is triggered by
  // OnSwapCompositorFrame, which is the appropriate time to request pixel
  // readback for the frame that is about to be drawn. Once called, the callback
  // pointer is released.
  // TODO(wjmaclean): We should consider making this available in other view
  // types, such as RenderWidgetHostViewAura.
  void RegisterFrameSwappedCallback(scoped_ptr<base::Closure> callback);

  // RenderWidgetHostView implementation.
  void InitAsChild(gfx::NativeView parent_view) override;
  RenderWidgetHost* GetRenderWidgetHost() const override;
  void SetSize(const gfx::Size& size) override;
  void SetBounds(const gfx::Rect& rect) override;
  void Focus() override;
  bool HasFocus() const override;
  bool IsSurfaceAvailableForCopy() const override;
  void Show() override;
  void Hide() override;
  bool IsShowing() override;
  gfx::Rect GetViewBounds() const override;
  gfx::Vector2dF GetLastScrollOffset() const override;
  gfx::NativeView GetNativeView() const override;
  gfx::NativeViewId GetNativeViewId() const override;
  gfx::NativeViewAccessible GetNativeViewAccessible() override;
  void SetBackgroundColor(SkColor color) override;
  gfx::Size GetPhysicalBackingSize() const override;

  // RenderWidgetHostViewBase implementation.
  void InitAsPopup(RenderWidgetHostView* parent_host_view,
                   const gfx::Rect& bounds) override;
  void InitAsFullscreen(RenderWidgetHostView* reference_host_view) override;
  void MovePluginWindows(const std::vector<WebPluginGeometry>& moves) override;
  void UpdateCursor(const WebCursor& cursor) override;
  void SetIsLoading(bool is_loading) override;
  void TextInputStateChanged(
      const ViewHostMsg_TextInputState_Params& params) override;
  void ImeCancelComposition() override;
  void ImeCompositionRangeChanged(
      const gfx::Range& range,
      const std::vector<gfx::Rect>& character_bounds) override;
  void RenderProcessGone(base::TerminationStatus status,
                         int error_code) override;
  void Destroy() override;
  void SetTooltipText(const base::string16& tooltip_text) override;
  void SelectionChanged(const base::string16& text,
                        size_t offset,
                        const gfx::Range& range) override;
  void SelectionBoundsChanged(
      const ViewHostMsg_SelectionBounds_Params& params) override;
  void CopyFromCompositingSurface(
      const gfx::Rect& src_subrect,
      const gfx::Size& dst_size,
      const ReadbackRequestCallback& callback,
      const SkColorType preferred_color_type) override;
  void CopyFromCompositingSurfaceToVideoFrame(
      const gfx::Rect& src_subrect,
      const scoped_refptr<media::VideoFrame>& target,
      const base::Callback<void(const gfx::Rect&, bool)>& callback) override;
  bool CanCopyToVideoFrame() const override;
  bool HasAcceleratedSurface(const gfx::Size& desired_size) override;
  void OnSwapCompositorFrame(uint32_t output_surface_id,
                             scoped_ptr<cc::CompositorFrame> frame) override;
  // Since the URL of content rendered by this class is not displayed in
  // the URL bar, this method does not need an implementation.
  void ClearCompositorFrame() override {}
  void GetScreenInfo(blink::WebScreenInfo* results) override;
  bool GetScreenColorProfile(std::vector<char>* color_profile) override;
  gfx::Rect GetBoundsInRootWindow() override;
#if defined(USE_AURA)
  void ProcessAckedTouchEvent(const TouchEventWithLatencyInfo& touch,
                              InputEventAckState ack_result) override;
#endif  // defined(USE_AURA)
  bool LockMouse() override;
  void UnlockMouse() override;
  uint32_t GetSurfaceIdNamespace() override;
  void ProcessKeyboardEvent(const NativeWebKeyboardEvent& event) override;
  void ProcessMouseEvent(const blink::WebMouseEvent& event) override;
  void ProcessMouseWheelEvent(const blink::WebMouseWheelEvent& event) override;
  gfx::Point TransformPointToRootCoordSpace(const gfx::Point& point) override;

#if defined(OS_MACOSX)
  // RenderWidgetHostView implementation.
  void SetActive(bool active) override;
  void SetWindowVisibility(bool visible) override;
  void WindowFrameChanged() override;
  void ShowDefinitionForSelection() override;
  bool SupportsSpeech() const override;
  void SpeakSelection() override;
  bool IsSpeaking() const override;
  void StopSpeaking() override;

  // RenderWidgetHostViewBase implementation.
  bool PostProcessEventForPluginIme(
      const NativeWebKeyboardEvent& event) override;
#endif  // defined(OS_MACOSX)

  // RenderWidgetHostViewBase implementation.
  void LockCompositingSurface() override;
  void UnlockCompositingSurface() override;

#if defined(OS_WIN)
  void SetParentNativeViewAccessible(
      gfx::NativeViewAccessible accessible_parent) override;
  gfx::NativeViewId GetParentForWindowlessPlugin() const override;
#endif
  BrowserAccessibilityManager* CreateBrowserAccessibilityManager(
      BrowserAccessibilityDelegate* delegate) override;

  // cc::SurfaceFactoryClient implementation.
  void ReturnResources(const cc::ReturnedResourceArray& resources) override;
  void SetBeginFrameSource(cc::SurfaceId surface_id,
                           cc::BeginFrameSource* begin_frame_source) override;

  // Declared 'public' instead of 'protected' here to allow derived classes
  // to Bind() to it.
  void SurfaceDrawn(uint32_t output_surface_id, cc::SurfaceDrawStatus drawn);

  // Exposed for tests.
  cc::SurfaceId SurfaceIdForTesting() const override;
  CrossProcessFrameConnector* FrameConnectorForTesting() const {
    return frame_connector_;
  }

 protected:
  friend class RenderWidgetHostView;
  friend class RenderWidgetHostViewChildFrameTest;
  friend class RenderWidgetHostViewGuestSurfaceTest;

  // Clears current compositor surface, if one is in use.
  void ClearCompositorSurfaceIfNecessary();

  void ProcessFrameSwappedCallbacks();

  // The last scroll offset of the view.
  gfx::Vector2dF last_scroll_offset_;

  // Members will become private when RenderWidgetHostViewGuest is removed.
  // The model object.
  RenderWidgetHostImpl* host_;

  // Surface-related state.
  scoped_ptr<cc::SurfaceIdAllocator> id_allocator_;
  scoped_ptr<cc::SurfaceFactory> surface_factory_;
  cc::SurfaceId surface_id_;
  uint32_t next_surface_sequence_;
  uint32_t last_output_surface_id_;
  gfx::Size current_surface_size_;
  float current_surface_scale_factor_;
  uint32_t ack_pending_count_;
  cc::ReturnedResourceArray surface_returned_resources_;

  // frame_connector_ provides a platform abstraction. Messages
  // sent through it are routed to the embedding renderer process.
  CrossProcessFrameConnector* frame_connector_;

  base::WeakPtr<RenderWidgetHostViewChildFrame> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  void SubmitSurfaceCopyRequest(const gfx::Rect& src_subrect,
                                const gfx::Size& dst_size,
                                const ReadbackRequestCallback& callback,
                                const SkColorType preferred_color_type);

  using FrameSwappedCallbackList = std::deque<scoped_ptr<base::Closure>>;
  // Since frame-drawn callbacks are "fire once", we use std::deque to make
  // it convenient to swap() when processing the list.
  FrameSwappedCallbackList frame_swapped_callbacks_;

  base::WeakPtrFactory<RenderWidgetHostViewChildFrame> weak_factory_;
  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostViewChildFrame);
};

}  // namespace content

#endif  // CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_CHILD_FRAME_H_

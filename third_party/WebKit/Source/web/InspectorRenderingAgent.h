// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef InspectorRenderingAgent_h
#define InspectorRenderingAgent_h

#include "core/inspector/InspectorBaseAgent.h"

namespace blink {

class InspectorOverlay;
class WebLocalFrameImpl;
class WebViewImpl;

using ErrorString = String;

class InspectorRenderingAgent final : public InspectorBaseAgent<InspectorRenderingAgent, protocol::Frontend::Rendering>, public protocol::Dispatcher::RenderingCommandHandler {
    WTF_MAKE_NONCOPYABLE(InspectorRenderingAgent);
public:
    static PassOwnPtrWillBeRawPtr<InspectorRenderingAgent> create(WebLocalFrameImpl*, InspectorOverlay*);

    // protocol::Dispatcher::PageCommandHandler implementation.
    void setShowPaintRects(ErrorString*, bool show) override;
    void setShowDebugBorders(ErrorString*, bool show) override;
    void setShowFPSCounter(ErrorString*, bool show) override;
    void setShowScrollBottleneckRects(ErrorString*, bool show) override;
    void setShowViewportSizeOnResize(ErrorString*, bool show) override;

    // InspectorBaseAgent overrides.
    void disable(ErrorString*) override;
    void restore() override;

    DECLARE_VIRTUAL_TRACE();

private:
    InspectorRenderingAgent(WebLocalFrameImpl*, InspectorOverlay*);
    bool compositingEnabled(ErrorString*);
    WebViewImpl* webViewImpl();

    RawPtrWillBeMember<WebLocalFrameImpl> m_webLocalFrameImpl;
    RawPtrWillBeMember<InspectorOverlay> m_overlay;
};


} // namespace blink


#endif // !defined(InspectorRenderingAgent_h)

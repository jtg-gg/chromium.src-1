// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ImageBitmapRenderingContext_h
#define ImageBitmapRenderingContext_h

#include "core/html/canvas/CanvasRenderingContext.h"
#include "core/html/canvas/CanvasRenderingContextFactory.h"
#include "modules/ModulesExport.h"
#include "wtf/RefPtr.h"

namespace blink {

class ImageBitmap;

class MODULES_EXPORT ImageBitmapRenderingContext final : public CanvasRenderingContext {
    DEFINE_WRAPPERTYPEINFO();
public:
    class Factory : public CanvasRenderingContextFactory {
        WTF_MAKE_NONCOPYABLE(Factory);
    public:
        Factory() {}
        ~Factory() override {}

        PassOwnPtrWillBeRawPtr<CanvasRenderingContext> create(HTMLCanvasElement*, const CanvasContextCreationAttributes&, Document&) override;
        CanvasRenderingContext::ContextType contextType() const override { return CanvasRenderingContext::ContextImageBitmap; }
        void onError(HTMLCanvasElement*, const String& error) override { }
    };

    // Script API
    void transferImageBitmap(ImageBitmap*);

    // CanvasRenderingContext implementation
    ContextType contextType() const override { return CanvasRenderingContext::ContextImageBitmap; }
    bool hasAlpha() const override { return m_hasAlpha; }
    void setIsHidden(bool) override { }
    bool isContextLost() const override { return false; }
    bool paint(GraphicsContext&, const IntRect&) override;

    // TODO(junov): Implement GPU accelerated rendering using a layer bridge
    WebLayer* platformLayer() const override { return nullptr; }
    // TODO(junov): handle lost contexts when content is GPU-backed
    void loseContext(LostContextMode) override { }

    void stop() override;

    virtual ~ImageBitmapRenderingContext();

private:
    ImageBitmapRenderingContext(HTMLCanvasElement*, CanvasContextCreationAttributes, Document&);

    bool m_hasAlpha;
    RefPtr<Image> m_image;
};

} // blink

#endif

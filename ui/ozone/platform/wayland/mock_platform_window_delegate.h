// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/platform_window/platform_window_delegate.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "ui/gfx/geometry/rect.h"

namespace ui {

class MockPlatformWindowDelegate : public PlatformWindowDelegate {
 public:
  MockPlatformWindowDelegate();
  ~MockPlatformWindowDelegate();

  MOCK_METHOD1(OnBoundsChanged, void(const gfx::Rect& new_bounds));
  MOCK_METHOD1(OnDamageRect, void(const gfx::Rect& damaged_region));
  MOCK_METHOD1(DispatchEvent, void(Event* event));
  MOCK_METHOD0(OnCloseRequest, void());
  MOCK_METHOD0(OnClosed, void());
  MOCK_METHOD1(OnWindowStateChanged, void(PlatformWindowState new_state));
  MOCK_METHOD0(OnLostCapture, void());
  MOCK_METHOD2(OnAcceleratedWidgetAvailable,
               void(gfx::AcceleratedWidget widget, float device_pixel_ratio));
  MOCK_METHOD0(OnAcceleratedWidgetDestroyed, void());
  MOCK_METHOD1(OnActivationChanged, void(bool active));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockPlatformWindowDelegate);
};

}  // namespace ui

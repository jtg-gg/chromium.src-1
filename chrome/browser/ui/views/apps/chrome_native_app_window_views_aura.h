// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_APPS_CHROME_NATIVE_APP_WINDOW_VIEWS_AURA_H_
#define CHROME_BROWSER_UI_VIEWS_APPS_CHROME_NATIVE_APP_WINDOW_VIEWS_AURA_H_

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/apps/chrome_native_app_window_views.h"

namespace apps {
class AppWindowFrameView;
}

// Aura-specific parts of ChromeNativeAppWindowViews. This is used directly on
// Linux and Windows, and is the base class for the Ash specific class used on
// ChromeOS.
class ChromeNativeAppWindowViewsAura : public ChromeNativeAppWindowViews {
 public:
  ChromeNativeAppWindowViewsAura();
  ~ChromeNativeAppWindowViewsAura() override;

 protected:
  ui::WindowShowState GetRestorableState(
      const ui::WindowShowState restore_state) const;

  // ChromeNativeAppWindowViews implementation.
  void OnBeforeWidgetInit(
      const extensions::AppWindow::CreateParams& create_params,
      views::Widget::InitParams* init_params,
      views::Widget* widget) override;
  views::NonClientFrameView* CreateNonStandardAppFrame() override;

  // ui::BaseWindow implementation.
  ui::WindowShowState GetRestoredState() const override;
  bool IsAlwaysOnTop() const override;

  // NativeAppWindow implementation.
  void UpdateShape(scoped_ptr<SkRegion> region) override;
  bool NWCanClose(bool user_force = false) override;

 private:
   enum CancelDownloadConfirmationState {
     NOT_PROMPTED,          // We have not asked the user.
     WAITING_FOR_RESPONSE,  // We have asked the user and have not received a
     // reponse yet.
     RESPONSE_RECEIVED      // The user was prompted and made a decision already.
   };
   CancelDownloadConfirmationState cancel_download_confirmation_state_;

   bool CanCloseWithInProgressDownloads();
   Browser::DownloadClosePreventionType OkToCloseWithInProgressDownloads(int* num_downloads_blocking) const;
   void InProgressDownloadResponse(bool cancel_downloads);

  FRIEND_TEST_ALL_PREFIXES(ShapedAppWindowTargeterTest,
                           ResizeInsetsWithinBounds);

  DISALLOW_COPY_AND_ASSIGN(ChromeNativeAppWindowViewsAura);
};

#endif  // CHROME_BROWSER_UI_VIEWS_APPS_CHROME_NATIVE_APP_WINDOW_VIEWS_AURA_H_

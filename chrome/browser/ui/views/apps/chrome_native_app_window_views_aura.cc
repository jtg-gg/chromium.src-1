// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/apps/chrome_native_app_window_views_aura.h"

#include <utility>

#include "apps/ui/views/app_window_frame_view.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "chrome/browser/apps/app_window_registry_util.h"
#include "chrome/browser/download/download_service.h"
#include "chrome/browser/ui/host_desktop.h"
#include "chrome/browser/ui/views/apps/app_window_easy_resize_window_targeter.h"
#include "chrome/browser/ui/views/apps/shaped_app_window_targeter.h"
#include "chrome/browser/ui/views/download/download_in_progress_dialog_view.h"
#include "chrome/browser/web_applications/web_app.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#include "ui/base/models/simple_menu_model.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/widget/widget.h"

#if defined(OS_LINUX)
#include "chrome/browser/shell_integration_linux.h"
#endif

using extensions::AppWindow;

ChromeNativeAppWindowViewsAura::ChromeNativeAppWindowViewsAura()
    : cancel_download_confirmation_state_(NOT_PROMPTED) {
}

ChromeNativeAppWindowViewsAura::~ChromeNativeAppWindowViewsAura() {
}

ui::WindowShowState
ChromeNativeAppWindowViewsAura::GetRestorableState(
    const ui::WindowShowState restore_state) const {
  // Whitelist states to return so that invalid and transient states
  // are not saved and used to restore windows when they are recreated.
  switch (restore_state) {
    case ui::SHOW_STATE_NORMAL:
    case ui::SHOW_STATE_MAXIMIZED:
    case ui::SHOW_STATE_FULLSCREEN:
      return restore_state;

    case ui::SHOW_STATE_DEFAULT:
    case ui::SHOW_STATE_MINIMIZED:
    case ui::SHOW_STATE_INACTIVE:
    case ui::SHOW_STATE_DOCKED:
    case ui::SHOW_STATE_END:
      return ui::SHOW_STATE_NORMAL;
  }

  return ui::SHOW_STATE_NORMAL;
}

void ChromeNativeAppWindowViewsAura::OnBeforeWidgetInit(
    const AppWindow::CreateParams& create_params,
    views::Widget::InitParams* init_params,
    views::Widget* widget) {
#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
  std::string app_name = web_app::GenerateApplicationNameFromExtensionId(
      app_window()->extension_id());
  // Set up a custom WM_CLASS for app windows. This allows task switchers in
  // X11 environments to distinguish them from main browser windows.
  init_params->wm_class_name = web_app::GetWMClassFromAppName(app_name);
  init_params->wm_class_class = shell_integration_linux::GetProgramClassName();
  const char kX11WindowRoleApp[] = "app";
  init_params->wm_role_name = std::string(kX11WindowRoleApp);
#endif

  ChromeNativeAppWindowViews::OnBeforeWidgetInit(create_params, init_params,
                                                 widget);
}

views::NonClientFrameView*
ChromeNativeAppWindowViewsAura::CreateNonStandardAppFrame() {
  apps::AppWindowFrameView* frame =
      new apps::AppWindowFrameView(widget(), this, HasFrameColor(),
                                   ActiveFrameColor(), InactiveFrameColor());
  frame->Init();

  // Install an easy resize window targeter, which ensures that the root window
  // (not the app) receives mouse events on the edges.
  aura::Window* window = widget()->GetNativeWindow();
  int resize_inside = frame->resize_inside_bounds_size();
  gfx::Insets inset(resize_inside, resize_inside, resize_inside, resize_inside);
  // Add the AppWindowEasyResizeWindowTargeter on the window, not its root
  // window. The root window does not have a delegate, which is needed to
  // handle the event in Linux.
  window->SetEventTargeter(scoped_ptr<ui::EventTargeter>(
      new AppWindowEasyResizeWindowTargeter(window, inset, this)));

  return frame;
}

ui::WindowShowState ChromeNativeAppWindowViewsAura::GetRestoredState() const {
  // Use kRestoreShowStateKey in case a window is minimized/hidden.
  ui::WindowShowState restore_state = widget()->GetNativeWindow()->GetProperty(
      aura::client::kRestoreShowStateKey);

  // First normal states are checked.
  if (IsMaximized())
    return ui::SHOW_STATE_MAXIMIZED;
  if (IsFullscreen()) {
    return ui::SHOW_STATE_FULLSCREEN;
  }

  if (widget()->GetNativeWindow()->GetProperty(
          aura::client::kShowStateKey) == ui::SHOW_STATE_DOCKED ||
      widget()->GetNativeWindow()->GetProperty(
          aura::client::kRestoreShowStateKey) == ui::SHOW_STATE_DOCKED) {
    return ui::SHOW_STATE_DOCKED;
  }

  return GetRestorableState(restore_state);
}

bool ChromeNativeAppWindowViewsAura::IsAlwaysOnTop() const {
  return widget()->IsAlwaysOnTop();
}

void ChromeNativeAppWindowViewsAura::UpdateShape(scoped_ptr<SkRegion> region) {
  bool had_shape = !!shape();

  ChromeNativeAppWindowViews::UpdateShape(std::move(region));

  aura::Window* native_window = widget()->GetNativeWindow();
  if (shape() && !had_shape) {
    native_window->SetEventTargeter(scoped_ptr<ui::EventTargeter>(
        new ShapedAppWindowTargeter(native_window, this)));
  } else if (!shape() && had_shape) {
    native_window->SetEventTargeter(scoped_ptr<ui::EventTargeter>());
  }
}

Browser::DownloadClosePreventionType ChromeNativeAppWindowViewsAura::OkToCloseWithInProgressDownloads(
  int* num_downloads_blocking) const {
  DCHECK(num_downloads_blocking);
  *num_downloads_blocking = 0;

  // count the active window (not closing) by checking the window's visibility
  // windows are set to "invisible" moments before it is closed
  int notClosingWindow = 0;
  for (const auto nativeWindow : AppWindowRegistryUtil::GetAppNativeWindowList()) {
    if (nativeWindow->IsVisible())
      notClosingWindow++;
  }

  if (notClosingWindow > 1)
    return Browser::DOWNLOAD_CLOSE_OK;   // Not the last window; can definitely close.

  int total_download_count =
    DownloadService::NonMaliciousDownloadCountAllProfiles();
  if (total_download_count == 0)
    return Browser::DOWNLOAD_CLOSE_OK;   // No downloads; can definitely close.

  *num_downloads_blocking = total_download_count;
  return Browser::DOWNLOAD_CLOSE_BROWSER_SHUTDOWN;
}

void ChromeNativeAppWindowViewsAura::InProgressDownloadResponse(bool cancel_downloads) {
  if (cancel_downloads) {
    cancel_download_confirmation_state_ = RESPONSE_RECEIVED;
    Close();
    return;
  }

  // Sets the confirmation state to NOT_PROMPTED so that if the user tries to
  // close again we'll show the warning again.
  cancel_download_confirmation_state_ = NOT_PROMPTED;
}

bool ChromeNativeAppWindowViewsAura::CanCloseWithInProgressDownloads() {
  // If we've prompted, we need to hear from the user before we
  // can close.
  if (cancel_download_confirmation_state_ != NOT_PROMPTED)
    return cancel_download_confirmation_state_ != WAITING_FOR_RESPONSE;

  int num_downloads_blocking;
  Browser::DownloadClosePreventionType dialog_type =
    OkToCloseWithInProgressDownloads(&num_downloads_blocking);
  if (dialog_type == Browser::DOWNLOAD_CLOSE_OK)
    return true;

  // Closing this window will kill some downloads; prompt to make sure
  // that's ok.
  cancel_download_confirmation_state_ = WAITING_FOR_RESPONSE;
  DownloadInProgressDialogView::Show(
    GetNativeWindow(), num_downloads_blocking, dialog_type, false, 
    base::Bind(&ChromeNativeAppWindowViewsAura::InProgressDownloadResponse, 
    base::Unretained(this)));

  // Return false so the browser does not close.  We'll close if the user
  // confirms in the dialog.
  return false;
}

bool ChromeNativeAppWindowViewsAura::NWCanClose(bool user_force) {
  if (CanCloseWithInProgressDownloads())
    return ChromeNativeAppWindowViews::NWCanClose(user_force);
  return false;
}


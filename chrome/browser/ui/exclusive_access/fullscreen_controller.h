// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_EXCLUSIVE_ACCESS_FULLSCREEN_CONTROLLER_H_
#define CHROME_BROWSER_UI_EXCLUSIVE_ACCESS_FULLSCREEN_CONTROLLER_H_

#include <set>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_controller_base.h"
#include "components/content_settings/core/common/content_settings.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

class GURL;

namespace content {
class WebContents;
}

// There are two different kinds of fullscreen mode - "tab fullscreen" and
// "browser fullscreen". "Tab fullscreen" refers to a renderer-initiated
// fullscreen mode (eg: from a Flash plugin or via the JS fullscreen API),
// whereas "browser fullscreen" refers to the user putting the browser itself
// into fullscreen mode from the UI. The difference is that tab fullscreen has
// implications for how the contents of the tab render (eg: a video element may
// grow to consume the whole tab), whereas browser fullscreen mode doesn't.
// Therefore if a user forces an exit from tab fullscreen, we need to notify the
// tab so it can stop rendering in its fullscreen mode.
//
// For Flash, FullscreenController will auto-accept all permission requests for
// fullscreen, since the assumption is that the plugin handles this for us.
//
// FullscreenWithinTab Note:
// All fullscreen widgets are displayed within the tab contents area, and
// FullscreenController will expand the browser window so that the tab contents
// area fills the entire screen. However, special behavior applies when a tab is
// being screen-captured. First, the browser window will not be
// fullscreened. This allows the user to retain control of their desktop to work
// in other browser tabs or applications while the fullscreen view is displayed
// on a remote screen. Second, FullscreenController will auto-resize fullscreen
// widgets to that of the capture video resolution when they are hidden (e.g.,
// when a user has switched to another tab). This is both a performance and
// quality improvement since scaling and letterboxing steps can be skipped in
// the capture pipeline.

// This class implements fullscreen behaviour.
class FullscreenController : public ExclusiveAccessControllerBase {
 public:
  explicit FullscreenController(ExclusiveAccessManager* manager);
  ~FullscreenController() override;

  // Browser/User Fullscreen ///////////////////////////////////////////////////

  // Returns true if the window is currently fullscreen and was initially
  // transitioned to fullscreen by a browser (i.e., not tab-initiated) mode
  // transition.
  bool IsFullscreenForBrowser() const;

  // Returns true if Flash is providing the "exit from fullscreen" message.
  bool IsPrivilegedFullscreenForTab() const;

  void ToggleBrowserFullscreenMode();

  // Fullscreen mode with tab strip and toolbar shown.
  // Currently only supported on Mac.
  void ToggleBrowserFullscreenWithToolbar();

  // Extension API implementation uses this method to toggle fullscreen mode.
  // The extension's name is displayed in the full screen bubble UI to attribute
  // the cause of the full screen state change.
  void ToggleBrowserFullscreenModeWithExtension(const GURL& extension_url);

  // Tab/HTML/Flash Fullscreen /////////////////////////////////////////////////

  // Returns true if the browser window has/will fullscreen because of
  // tab-initiated fullscreen. The window may still be transitioning, and
  // BrowserWindow::IsFullscreen() may still return false.
  bool IsWindowFullscreenForTabOrPending() const;

  // Returns true if the browser window is fullscreen because of extension
  // initiated fullscreen.
  bool IsExtensionFullscreenOrPending() const;

  // Returns true if controller has entered fullscreen mode.
  bool IsControllerInitiatedFullscreen() const;

  // Returns true if the user has accepted fullscreen.
  bool IsUserAcceptedFullscreen() const;

  // Returns true if the tab is/will be in fullscreen mode. Note: This does NOT
  // indicate whether the browser window is/will be fullscreened as well. See
  // 'FullscreenWithinTab Note'.
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) const;

  // True if fullscreen was entered because of tab fullscreen (was not
  // previously in user-initiated fullscreen).
  bool IsFullscreenCausedByTab() const;

  // Enter tab-initiated fullscreen mode. FullscreenController will decide
  // whether to also fullscreen the browser window. See 'FullscreenWithinTab
  // Note'.
  // |web_contents| represents the tab that requests to be fullscreen.
  // |origin| represents the origin of the requesting frame inside the
  // WebContents. If empty, then the |web_contents|'s latest committed URL
  // origin will be used.
  void EnterFullscreenModeForTab(content::WebContents* web_contents,
                                 const GURL& origin);

  // Leave a tab-initiated fullscreen mode.
  // |web_contents| represents the tab that requests to no longer be fullscreen.
  void ExitFullscreenModeForTab(content::WebContents* web_contents);

  // Platform Fullscreen ///////////////////////////////////////////////////////

#if defined(OS_WIN)
  // Returns whether we are currently in a Metro snap view.
  bool IsInMetroSnapMode();

  // API that puts the window into a mode suitable for rendering when Chrome
  // is rendered in a 20% screen-width Metro snap view on Windows 8.
  void SetMetroSnapMode(bool enable);
#endif  // OS_WIN

  // Overrde from ExclusiveAccessControllerBase.
  void OnTabDetachedFromView(content::WebContents* web_contents) override;
  void OnTabClosing(content::WebContents* web_contents) override;
  bool HandleUserPressedEscape() override;

  void ExitExclusiveAccessToPreviousState() override;
  bool OnAcceptExclusiveAccessPermission() override;
  bool OnDenyExclusiveAccessPermission() override;
  GURL GetURLForExclusiveAccessBubble() const override;
  void ExitExclusiveAccessIfNecessary() override;
  // Callbacks /////////////////////////////////////////////////////////////////

  // Called by Browser::WindowFullscreenStateChanged.
  void WindowFullscreenStateChanged();

 private:
  friend class FullscreenControllerTest;

  enum FullscreenInternalOption {
    BROWSER,
    BROWSER_WITH_TOOLBAR,
    TAB
  };

  // Posts a task to call NotifyFullscreenChange.
  void PostFullscreenChangeNotification(bool is_fullscreen);
  // Sends a NOTIFICATION_FULLSCREEN_CHANGED notification.
  void NotifyFullscreenChange(bool is_fullscreen);

  // Notifies the tab that it has been forced out of fullscreen mode if
  // necessary.
  void NotifyTabExclusiveAccessLost() override;

  void RecordBubbleReshowsHistogram(int bubble_reshow_count) override;

  void ToggleFullscreenModeInternal(FullscreenInternalOption option);
  void EnterFullscreenModeInternal(FullscreenInternalOption option);
  void ExitFullscreenModeInternal();
  void SetFullscreenedTab(content::WebContents* tab, const GURL& origin);

  ContentSetting GetFullscreenSetting() const;

  void SetPrivilegedFullscreenForTesting(bool is_privileged);
  // Returns true if |web_contents| was toggled into/out of fullscreen mode as a
  // screen-captured tab. See 'FullscreenWithinTab Note'.
  bool MaybeToggleFullscreenForCapturedTab(content::WebContents* web_contents,
                                           bool enter_fullscreen);
  // Returns true if |web_contents| is in fullscreen mode as a screen-captured
  // tab. See 'FullscreenWithinTab Note'.
  bool IsFullscreenForCapturedTab(const content::WebContents* web_contents)
      const;

  // Helper methods that should be used in a TAB context.
  GURL GetRequestingOrigin() const;
  GURL GetEmbeddingOrigin() const;

  // If a tab is fullscreen, the |fullscreen_origin_| should be used as the
  // origin with regards to fullscreen. The |fullscreened_tab_| url should be
  // used as the embedder url.
  GURL fullscreened_origin_;

  // The URL of the extension which trigerred "browser fullscreen" mode.
  GURL extension_caused_fullscreen_;

  enum PriorFullscreenState {
    STATE_INVALID,
    STATE_NORMAL,
    STATE_BROWSER_FULLSCREEN_NO_TOOLBAR,
    STATE_BROWSER_FULLSCREEN_WITH_TOOLBAR,
  };
  // The state before entering tab fullscreen mode via webkitRequestFullScreen.
  // When not in tab fullscreen, it is STATE_INVALID.
  PriorFullscreenState state_prior_to_tab_fullscreen_;
  // True if tab fullscreen has been allowed, either by settings or by user
  // clicking the allow button on the fullscreen infobar.
  bool tab_fullscreen_accepted_;

  // True if this controller has toggled into tab OR browser fullscreen.
  bool toggled_into_fullscreen_;

  // Used to verify that calls we expect to reenter by calling
  // WindowFullscreenStateChanged do so.
  bool reentrant_window_state_change_call_check_;

  // Used in testing to confirm proper behavior for specific, privileged
  // fullscreen cases.
  bool is_privileged_fullscreen_for_testing_;

  base::WeakPtrFactory<FullscreenController> ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(FullscreenController);
};

#endif  // CHROME_BROWSER_UI_EXCLUSIVE_ACCESS_FULLSCREEN_CONTROLLER_H_

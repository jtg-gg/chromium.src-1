// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/apps/app_window_registry_util.h"

#include <vector>

#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "extensions/browser/app_window/app_window.h"
#include "extensions/browser/app_window/app_window_registry.h"
#include "extensions/browser/app_window/native_app_window.h"

using extensions::AppWindow;
using extensions::AppWindowRegistry;

typedef AppWindowRegistry::AppWindowList AppWindowList;
typedef AppWindowRegistry::Factory Factory;

// static
AppWindow* AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
    gfx::NativeWindow window) {
  std::vector<Profile*> profiles =
      g_browser_process->profile_manager()->GetLoadedProfiles();
  for (std::vector<Profile*>::const_iterator i = profiles.begin();
       i != profiles.end();
       ++i) {
    AppWindowRegistry* registry =
        Factory::GetForBrowserContext(*i, false /* create */);
    if (!registry)
      continue;

    AppWindow* app_window = registry->GetAppWindowForNativeWindow(window);
    if (app_window)
      return app_window;
  }

  return NULL;
}

// static
bool AppWindowRegistryUtil::IsAppWindowVisibleInAnyProfile(
                                                           int window_type_mask, bool check_visible) {
  std::vector<Profile*> profiles =
      g_browser_process->profile_manager()->GetLoadedProfiles();
  for (std::vector<Profile*>::const_iterator i = profiles.begin();
       i != profiles.end();
       ++i) {
    AppWindowRegistry* registry =
        Factory::GetForBrowserContext(*i, false /* create */);
    if (!registry)
      continue;

    const AppWindowList& app_windows = registry->app_windows();
    if (app_windows.empty())
      continue;

    for (const AppWindow* window : app_windows) {
      if ((!window->is_hidden() || !check_visible )&&
          (window_type_mask == 0 ||
           (window->window_type() & window_type_mask)))
        return true;
    }
  }

  return false;
}

// static
bool AppWindowRegistryUtil::CloseAllAppWindows(bool user_force) {
  std::vector<Profile*> profiles =
      g_browser_process->profile_manager()->GetLoadedProfiles();
  for (std::vector<Profile*>::const_iterator i = profiles.begin();
       i != profiles.end();
       ++i) {
    AppWindowRegistry* registry =
        Factory::GetForBrowserContext(*i, false /* create */);
    if (!registry)
      continue;

    // Ask each app window to close, but cater for windows removing or
    // rearranging themselves in the ordered window list in response.
    AppWindowList window_list_copy(registry->app_windows());
    for (const auto& window : window_list_copy) {
      // Ensure window is still valid.
      if (std::find(registry->app_windows().begin(),
                    registry->app_windows().end(),
                    window) != registry->app_windows().end()) {
        extensions::NativeAppWindow *nativeAppWindow = window->GetBaseWindow();
        if (nativeAppWindow->NWCanClose(user_force))
          nativeAppWindow->Close();
        else
          return false;
      }
    }
  }
  return true;
}

//static
AppWindowRegistryUtil::NativeWindowList AppWindowRegistryUtil::GetAppNativeWindowList() {
  std::vector<Profile*> profiles =
    g_browser_process->profile_manager()->GetLoadedProfiles();
  AppWindowRegistryUtil::NativeWindowList nativeWindowList;
  for (std::vector<Profile*>::const_iterator i = profiles.begin();
    i != profiles.end();
    ++i) {
    AppWindowRegistry* registry =
      Factory::GetForBrowserContext(*i, false /* create */);
    if (!registry)
      continue;

    const AppWindowList& app_windows = registry->app_windows();
    if (app_windows.empty())
      continue;

    for (AppWindow* window : app_windows) {
      nativeWindowList.push_back(window->GetNativeWindow());
    }
  }

  return nativeWindowList;

}

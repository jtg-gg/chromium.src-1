// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/platform_util.h"

#include "base/bind.h"
#include "base/files/file_path.h"
#include "chrome/browser/chromeos/file_manager/open_util.h"
#include "chrome/browser/platform_util_internal.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/grit/generated_resources.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

using content::BrowserThread;

namespace platform_util {

namespace {

const char kGmailComposeUrl[] =
    "https://mail.google.com/mail/?extsrc=mailto&url=";

void ShowWarningOnOpenOperationResult(Profile* profile,
                                      const base::FilePath& path,
                                      OpenOperationResult result) {
  int message_id = IDS_FILE_BROWSER_ERROR_VIEWING_FILE;
  switch (result) {
    case OPEN_SUCCEEDED:
      return;

    case OPEN_FAILED_PATH_NOT_FOUND:
      message_id = IDS_FILE_BROWSER_ERROR_UNRESOLVABLE_FILE;
      break;

    case OPEN_FAILED_INVALID_TYPE:
      return;

    case OPEN_FAILED_NO_HANLDER_FOR_FILE_TYPE:
      if (path.MatchesExtension(FILE_PATH_LITERAL(".dmg")))
        message_id = IDS_FILE_BROWSER_ERROR_VIEWING_FILE_FOR_DMG;
      else if (path.MatchesExtension(FILE_PATH_LITERAL(".exe")) ||
               path.MatchesExtension(FILE_PATH_LITERAL(".msi")))
        message_id = IDS_FILE_BROWSER_ERROR_VIEWING_FILE_FOR_EXECUTABLE;
      else
        message_id = IDS_FILE_BROWSER_ERROR_VIEWING_FILE;
      break;

    case OPEN_FAILED_FILE_ERROR:
      message_id = IDS_FILE_BROWSER_ERROR_VIEWING_FILE;
      break;
  }

  Browser* browser = chrome::FindTabbedBrowser(profile, false);
  chrome::ShowMessageBox(
      browser ? browser->window()->GetNativeWindow() : nullptr,
      l10n_util::GetStringFUTF16(IDS_FILE_BROWSER_ERROR_VIEWING_FILE_TITLE,
                                 path.BaseName().AsUTF16Unsafe()),
      l10n_util::GetStringUTF16(message_id), chrome::MESSAGE_BOX_TYPE_WARNING);
}

}  // namespace

namespace internal {

void DisableShellOperationsForTesting() {
  file_manager::util::DisableShellOperationsForTesting();
}

}  // namespace internal

void ShowItemInFolder(Profile* profile, const base::FilePath& full_path) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  file_manager::util::ShowItemInFolder(
      profile, full_path,
      base::Bind(&ShowWarningOnOpenOperationResult, profile, full_path));
}

void OpenItem(Profile* profile,
              const base::FilePath& full_path,
              OpenItemType item_type,
              const OpenOperationCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  file_manager::util::OpenItem(
      profile, full_path, item_type,
      callback.is_null()
          ? base::Bind(&ShowWarningOnOpenOperationResult, profile, full_path)
          : callback);
}

void OpenExternal(Profile* profile, const GURL& url) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // This code should be obsolete since we have default handlers in ChromeOS
  // which should handle this. However - there are two things which make it
  // necessary to keep it in:
  // a.) The user might have deleted the default handler in this session.
  //     In this case we would need to have this in place.
  // b.) There are several code paths which are not clear if they would call
  //     this function directly and which would therefore break (e.g.
  //     "Browser::EmailPageLocation" (to name only one).
  // As such we should keep this code here.
  chrome::NavigateParams params(profile, url, ui::PAGE_TRANSITION_LINK);
  params.disposition = NEW_FOREGROUND_TAB;
  params.host_desktop_type = chrome::HOST_DESKTOP_TYPE_ASH;

  if (url.SchemeIs("mailto")) {
    std::string string_url = kGmailComposeUrl;
    string_url.append(url.spec());
    params.url = GURL(url);
  }

  chrome::Navigate(&params);
}

}  // namespace platform_util

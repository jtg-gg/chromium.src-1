// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/launcher.h"

#include <set>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/extensions/api/file_handlers/app_file_handler_util.h"
#include "chrome/browser/extensions/api/file_handlers/directory_util.h"
#include "chrome/browser/extensions/api/file_handlers/mime_util.h"
#include "chrome/browser/extensions/api/file_system/file_system_api.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "extensions/browser/api/app_runtime/app_runtime_api.h"
#include "extensions/browser/entry_info.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_host.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/granted_file_entry.h"
#include "extensions/browser/lazy_background_task_queue.h"
#include "extensions/browser/process_manager.h"
#include "extensions/common/api/app_runtime.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/kiosk_mode_info.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"

#if defined(OS_CHROMEOS)
#include "components/user_manager/user_manager.h"
#endif

namespace app_runtime = extensions::api::app_runtime;

using content::BrowserThread;
using extensions::AppRuntimeEventRouter;
using extensions::app_file_handler_util::CreateFileEntry;
using extensions::app_file_handler_util::FileHandlerCanHandleEntry;
using extensions::app_file_handler_util::FileHandlerForId;
using extensions::app_file_handler_util::FirstFileHandlerForEntry;
using extensions::app_file_handler_util::HasFileSystemWritePermission;
using extensions::app_file_handler_util::PrepareFilesForWritableApp;
using extensions::EventRouter;
using extensions::Extension;
using extensions::ExtensionHost;
using extensions::GrantedFileEntry;

namespace apps {

namespace {

const char kFallbackMimeType[] = "application/octet-stream";

bool DoMakePathAbsolute(const base::FilePath& current_directory,
                        base::FilePath* file_path) {
  DCHECK(file_path);
  if (file_path->IsAbsolute())
    return true;

  if (current_directory.empty()) {
    base::FilePath absolute_path = base::MakeAbsoluteFilePath(*file_path);
    if (absolute_path.empty())
      return false;
    *file_path = absolute_path;
    return true;
  }

  if (!current_directory.IsAbsolute())
    return false;

  *file_path = current_directory.Append(*file_path);
  return true;
}

// Class to handle launching of platform apps to open specific paths.
// An instance of this class is created for each launch. The lifetime of these
// instances is managed by reference counted pointers. As long as an instance
// has outstanding tasks on a message queue it will be retained; once all
// outstanding tasks are completed it will be deleted.
class PlatformAppPathLauncher
    : public base::RefCountedThreadSafe<PlatformAppPathLauncher> {
 public:
  PlatformAppPathLauncher(Profile* profile,
                          const Extension* extension,
                          const std::vector<base::FilePath>& entry_paths)
      : profile_(profile),
        extension_id(extension->id()),
        entry_paths_(entry_paths),
        mime_type_collector_(profile),
        is_directory_collector_(profile) {
    if (extension->is_nwjs_app()) //NWJS#5097
      entry_paths_.clear();
  }

  PlatformAppPathLauncher(Profile* profile,
                          const Extension* extension,
                          const base::FilePath& file_path)
      : profile_(profile),
        extension_id(extension->id()),
        mime_type_collector_(profile),
        is_directory_collector_(profile) {
    if (!file_path.empty() && !extension->is_nwjs_app()) //NWJS#5097
      entry_paths_.push_back(file_path);
  }

  void Launch() {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);

    const Extension* extension = GetExtension();
    if (!extension)
      return;

    if (entry_paths_.empty()) {
      LaunchWithNoLaunchData();
      return;
    }

    for (size_t i = 0; i < entry_paths_.size(); ++i) {
      DCHECK(entry_paths_[i].IsAbsolute());
    }

    is_directory_collector_.CollectForEntriesPaths(
        entry_paths_,
        base::Bind(&PlatformAppPathLauncher::OnAreDirectoriesCollected, this,
                   HasFileSystemWritePermission(extension)));
  }

  void LaunchWithHandler(const std::string& handler_id) {
    handler_id_ = handler_id;
    Launch();
  }

  void LaunchWithRelativePath(const base::FilePath& current_directory) {
    BrowserThread::PostTask(
        BrowserThread::FILE,
        FROM_HERE,
        base::Bind(&PlatformAppPathLauncher::MakePathAbsolute,
                   this,
                   current_directory));
  }

 private:
  friend class base::RefCountedThreadSafe<PlatformAppPathLauncher>;

  virtual ~PlatformAppPathLauncher() {}

  void MakePathAbsolute(const base::FilePath& current_directory) {
    DCHECK_CURRENTLY_ON(BrowserThread::FILE);

    for (std::vector<base::FilePath>::iterator it = entry_paths_.begin();
         it != entry_paths_.end(); ++it) {
      if (!DoMakePathAbsolute(current_directory, &*it)) {
        LOG(WARNING) << "Cannot make absolute path from " << it->value();
        BrowserThread::PostTask(
            BrowserThread::UI,
            FROM_HERE,
            base::Bind(&PlatformAppPathLauncher::LaunchWithNoLaunchData, this));
        return;
      }
    }

    BrowserThread::PostTask(BrowserThread::UI,
                            FROM_HERE,
                            base::Bind(&PlatformAppPathLauncher::Launch, this));
  }

  void OnFilesValid(scoped_ptr<std::set<base::FilePath>> directory_paths) {
    mime_type_collector_.CollectForLocalPaths(
        entry_paths_,
        base::Bind(
            &PlatformAppPathLauncher::OnAreDirectoriesAndMimeTypesCollected,
            this, base::Passed(std::move(directory_paths))));
  }

  void OnFilesInvalid(const base::FilePath& /* error_path */) {
    LaunchWithNoLaunchData();
  }

  void LaunchWithNoLaunchData() {
    // This method is required as an entry point on the UI thread.
    DCHECK_CURRENTLY_ON(BrowserThread::UI);

    const Extension* extension = GetExtension();
    if (!extension)
      return;

    AppRuntimeEventRouter::DispatchOnLaunchedEvent(
        profile_, extension, extensions::SOURCE_FILE_HANDLER);
  }

  void OnAreDirectoriesCollected(
      bool has_file_system_write_permission,
      scoped_ptr<std::set<base::FilePath>> directory_paths) {
    if (has_file_system_write_permission) {
      std::set<base::FilePath>* const directory_paths_ptr =
          directory_paths.get();
      PrepareFilesForWritableApp(
          entry_paths_, profile_, *directory_paths_ptr,
          base::Bind(&PlatformAppPathLauncher::OnFilesValid, this,
                     base::Passed(std::move(directory_paths))),
          base::Bind(&PlatformAppPathLauncher::OnFilesInvalid, this));
      return;
    }

    OnFilesValid(std::move(directory_paths));
  }

  void OnAreDirectoriesAndMimeTypesCollected(
      scoped_ptr<std::set<base::FilePath>> directory_paths,
      scoped_ptr<std::vector<std::string>> mime_types) {
    DCHECK(entry_paths_.size() == mime_types->size());
    // If fetching a mime type failed, then use a fallback one.
    for (size_t i = 0; i < entry_paths_.size(); ++i) {
      const std::string mime_type =
          !(*mime_types)[i].empty() ? (*mime_types)[i] : kFallbackMimeType;
      bool is_directory =
          directory_paths->find(entry_paths_[i]) != directory_paths->end();
      entries_.push_back(
          extensions::EntryInfo(entry_paths_[i], mime_type, is_directory));
    }

    const Extension* extension = GetExtension();
    if (!extension)
      return;

    // Find file handler from the platform app for the file being opened.
    const extensions::FileHandlerInfo* handler = NULL;
    if (!handler_id_.empty()) {
      handler = FileHandlerForId(*extension, handler_id_);
      if (handler) {
        for (size_t i = 0; i < entry_paths_.size(); ++i) {
          if (!FileHandlerCanHandleEntry(*handler, entries_[i])) {
            LOG(WARNING)
                << "Extension does not provide a valid file handler for "
                << entry_paths_[i].value();
            handler = NULL;
            break;
          }
        }
      }
    } else {
      const std::vector<const extensions::FileHandlerInfo*>& handlers =
          extensions::app_file_handler_util::FindFileHandlersForEntries(
              *extension, entries_);
      if (!handlers.empty())
        handler = handlers[0];
    }

    // If this app doesn't have a file handler that supports the file, launch
    // with no launch data.
    if (!handler) {
      LOG(WARNING) << "Extension does not provide a valid file handler.";
      LaunchWithNoLaunchData();
      return;
    }

    if (handler_id_.empty())
      handler_id_ = handler->id;

    // Access needs to be granted to the file for the process associated with
    // the extension. To do this the ExtensionHost is needed. This might not be
    // available, or it might be in the process of being unloaded, in which case
    // the lazy background task queue is used to load the extension and then
    // call back to us.
    extensions::LazyBackgroundTaskQueue* const queue =
        extensions::LazyBackgroundTaskQueue::Get(profile_);
    if (queue->ShouldEnqueueTask(profile_, extension)) {
      queue->AddPendingTask(
          profile_, extension_id,
          base::Bind(&PlatformAppPathLauncher::GrantAccessToFilesAndLaunch,
                     this));
      return;
    }

    extensions::ProcessManager* const process_manager =
        extensions::ProcessManager::Get(profile_);
    ExtensionHost* const host =
        process_manager->GetBackgroundHostForExtension(extension_id);
    DCHECK(host);
    GrantAccessToFilesAndLaunch(host);
  }

  void GrantAccessToFilesAndLaunch(ExtensionHost* host) {
    const Extension* extension = GetExtension();
    if (!extension)
      return;

    // If there was an error loading the app page, |host| will be NULL.
    if (!host) {
      LOG(ERROR) << "Could not load app page for " << extension_id;
      return;
    }

    std::vector<GrantedFileEntry> granted_entries;
    for (size_t i = 0; i < entry_paths_.size(); ++i) {
      granted_entries.push_back(CreateFileEntry(
          profile_, extension, host->render_process_host()->GetID(),
          entries_[i].path, entries_[i].is_directory));
    }

    AppRuntimeEventRouter::DispatchOnLaunchedEventWithFileEntries(
        profile_, extension, handler_id_, entries_, granted_entries);
  }

  const Extension* GetExtension() const {
    return extensions::ExtensionRegistry::Get(profile_)->GetExtensionById(
        extension_id, extensions::ExtensionRegistry::EVERYTHING);
  }

  // The profile the app should be run in.
  Profile* profile_;
  // The id of the extension providing the app. A pointer to the extension is
  // not kept as the extension may be unloaded and deleted during the course of
  // the launch.
  const std::string extension_id;
  // A list of files and directories to be passed through to the app.
  std::vector<base::FilePath> entry_paths_;
  // A corresponding list with EntryInfo for every base::FilePath in
  // entry_paths_.
  std::vector<extensions::EntryInfo> entries_;
  // The ID of the file handler used to launch the app.
  std::string handler_id_;
  extensions::app_file_handler_util::MimeTypeCollector mime_type_collector_;
  extensions::app_file_handler_util::IsDirectoryCollector
      is_directory_collector_;

  DISALLOW_COPY_AND_ASSIGN(PlatformAppPathLauncher);
};

}  // namespace

void LaunchPlatformAppWithCommandLine(Profile* profile,
                                      const Extension* extension,
                                      const base::CommandLine& command_line,
                                      const base::FilePath& current_directory,
                                      extensions::AppLaunchSource source) {
  // An app with "kiosk_only" should not be installed and launched
  // outside of ChromeOS kiosk mode in the first place. This is a defensive
  // check in case this scenario does occur.
  if (extensions::KioskModeInfo::IsKioskOnly(extension)) {
    bool in_kiosk_mode = false;
#if defined(OS_CHROMEOS)
    user_manager::UserManager* user_manager = user_manager::UserManager::Get();
    in_kiosk_mode = user_manager && user_manager->IsLoggedInAsKioskApp();
#endif
    if (!in_kiosk_mode) {
      LOG(ERROR) << "App with 'kiosk_only' attribute must be run in "
          << " ChromeOS kiosk mode.";
      NOTREACHED();
      return;
    }
  }

#if defined(OS_WIN)
  base::CommandLine::StringType about_blank_url(
      base::ASCIIToUTF16(url::kAboutBlankURL));
#else
  base::CommandLine::StringType about_blank_url(url::kAboutBlankURL);
#endif
  base::CommandLine::StringVector args = command_line.GetArgs();
  // Browser tests will add about:blank to the command line. This should
  // never be interpreted as a file to open, as doing so with an app that
  // has write access will result in a file 'about' being created, which
  // causes problems on the bots.
  if (args.empty() || (command_line.HasSwitch(switches::kTestType) &&
                       args[0] == about_blank_url)) {
    AppRuntimeEventRouter::DispatchOnLaunchedEvent(profile, extension, source);
    return;
  }

  base::FilePath file_path(command_line.GetArgs()[0]);
  scoped_refptr<PlatformAppPathLauncher> launcher =
      new PlatformAppPathLauncher(profile, extension, file_path);
  launcher->LaunchWithRelativePath(current_directory);
}

void LaunchPlatformAppWithPath(Profile* profile,
                               const Extension* extension,
                               const base::FilePath& file_path) {
  scoped_refptr<PlatformAppPathLauncher> launcher =
      new PlatformAppPathLauncher(profile, extension, file_path);
  launcher->Launch();
}

void LaunchPlatformApp(Profile* profile,
                       const Extension* extension,
                       extensions::AppLaunchSource source) {
  LaunchPlatformAppWithCommandLine(
      profile,
      extension,
      base::CommandLine(base::CommandLine::NO_PROGRAM),
      base::FilePath(),
      source);
}

void LaunchPlatformAppWithFileHandler(
    Profile* profile,
    const Extension* extension,
    const std::string& handler_id,
    const std::vector<base::FilePath>& entry_paths) {
  scoped_refptr<PlatformAppPathLauncher> launcher =
      new PlatformAppPathLauncher(profile, extension, entry_paths);
  launcher->LaunchWithHandler(handler_id);
}

void RestartPlatformApp(Profile* profile, const Extension* extension) {
  EventRouter* event_router = EventRouter::Get(profile);
  bool listening_to_restart = event_router->
      ExtensionHasEventListener(extension->id(),
                                app_runtime::OnRestarted::kEventName);

  if (listening_to_restart) {
    AppRuntimeEventRouter::DispatchOnRestartedEvent(profile, extension);
    return;
  }

  extensions::ExtensionPrefs* extension_prefs =
      extensions::ExtensionPrefs::Get(profile);
  bool had_windows = extension_prefs->IsActive(extension->id());
  extension_prefs->SetIsActive(extension->id(), false);
  bool listening_to_launch = event_router->
      ExtensionHasEventListener(extension->id(),
                                app_runtime::OnLaunched::kEventName);

  if (listening_to_launch && had_windows) {
    AppRuntimeEventRouter::DispatchOnLaunchedEvent(
        profile, extension, extensions::SOURCE_RESTART);
  }
}

void LaunchPlatformAppWithUrl(Profile* profile,
                              const Extension* extension,
                              const std::string& handler_id,
                              const GURL& url,
                              const GURL& referrer_url) {
  AppRuntimeEventRouter::DispatchOnLaunchedEventWithUrl(
      profile, extension, handler_id, url, referrer_url);
}

}  // namespace apps

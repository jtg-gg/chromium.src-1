// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/browser_state/chrome_browser_state_manager_impl.h"

#include <stdint.h>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/metrics/histogram_macros.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "components/browser_sync/browser/profile_sync_service.h"
#include "components/invalidation/impl/profile_invalidation_provider.h"
#include "components/password_manager/core/browser/password_store.h"
#include "components/password_manager/sync/browser/password_manager_setting_migrator_service.h"
#include "components/prefs/pref_service.h"
#include "components/signin/core/browser/account_fetcher_service.h"
#include "components/signin/core/browser/account_tracker_service.h"
#include "components/signin/core/browser/gaia_cookie_manager_service.h"
#include "components/signin/core/browser/signin_manager.h"
#include "ios/chrome/browser/application_context.h"
#include "ios/chrome/browser/browser_state/browser_state_info_cache.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state_impl.h"
#include "ios/chrome/browser/browser_state/off_the_record_chrome_browser_state_impl.h"
#include "ios/chrome/browser/browser_state_metrics/browser_state_metrics.h"
#include "ios/chrome/browser/chrome_constants.h"
#include "ios/chrome/browser/chrome_paths.h"
#include "ios/chrome/browser/data_reduction_proxy/ios_chrome_data_reduction_proxy_settings.h"
#include "ios/chrome/browser/data_reduction_proxy/ios_chrome_data_reduction_proxy_settings_factory.h"
#include "ios/chrome/browser/invalidation/ios_chrome_profile_invalidation_provider_factory.h"
#include "ios/chrome/browser/passwords/ios_chrome_password_manager_setting_migrator_service_factory.h"
#include "ios/chrome/browser/pref_names.h"
#include "ios/chrome/browser/signin/account_consistency_service_factory.h"
#include "ios/chrome/browser/signin/account_fetcher_service_factory.h"
#include "ios/chrome/browser/signin/account_reconcilor_factory.h"
#include "ios/chrome/browser/signin/account_tracker_service_factory.h"
#include "ios/chrome/browser/signin/gaia_cookie_manager_service_factory.h"
#include "ios/chrome/browser/signin/signin_manager_factory.h"
#include "ios/chrome/browser/sync/ios_chrome_profile_sync_service_factory.h"
#include "ios/web/public/active_state_manager.h"
#include "ios/web/public/web_thread.h"

namespace {

int64_t ComputeFilesSize(const base::FilePath& directory,
                         const base::FilePath::StringType& pattern) {
  int64_t running_size = 0;
  base::FileEnumerator iter(directory, false, base::FileEnumerator::FILES,
                            pattern);
  while (!iter.Next().empty())
    running_size += iter.GetInfo().GetSize();
  return running_size;
}

// Simple task to log the size of the browser state at |path|.
void BrowserStateSizeTask(const base::FilePath& path) {
  DCHECK_CURRENTLY_ON(web::WebThread::FILE);
  const int64_t kBytesInOneMB = 1024 * 1024;

  int64_t size = ComputeFilesSize(path, FILE_PATH_LITERAL("*"));
  int size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.TotalSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("History"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.HistorySize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("History*"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.TotalHistorySize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Cookies"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.CookiesSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Bookmarks"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.BookmarksSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Favicons"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.FaviconsSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Top Sites"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.TopSitesSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Visited Links"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.VisitedLinksSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Web Data"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.WebDataSize", size_MB);

  size = ComputeFilesSize(path, FILE_PATH_LITERAL("Extension*"));
  size_MB = static_cast<int>(size / kBytesInOneMB);
  UMA_HISTOGRAM_COUNTS_10000("Profile.ExtensionSize", size_MB);
}

// Gets the user data directory.
base::FilePath GetUserDataDir() {
  base::FilePath user_data_dir;
  bool result = PathService::Get(ios::DIR_USER_DATA, &user_data_dir);
  DCHECK(result);
  return user_data_dir;
}

}  // namespace

ChromeBrowserStateManagerImpl::ChromeBrowserStateManagerImpl() {}

ChromeBrowserStateManagerImpl::~ChromeBrowserStateManagerImpl() {
  for (const auto& pair : browser_states_) {
    ChromeBrowserStateImpl* browser_state = pair.second.get();
    web::BrowserState::GetActiveStateManager(browser_state)->SetActive(false);
    if (!browser_state->HasOffTheRecordChromeBrowserState())
      continue;

    web::BrowserState* otr_browser_state =
        browser_state->GetOffTheRecordChromeBrowserState();
    if (!web::BrowserState::HasActiveStateManager(otr_browser_state))
      continue;
    web::BrowserState::GetActiveStateManager(otr_browser_state)
        ->SetActive(false);
  }
}

ios::ChromeBrowserState*
ChromeBrowserStateManagerImpl::GetLastUsedBrowserState() {
  return GetBrowserState(GetLastUsedBrowserStateDir(GetUserDataDir()));
}

ios::ChromeBrowserState* ChromeBrowserStateManagerImpl::GetBrowserState(
    const base::FilePath& path) {
  // If the browser state is already loaded, just return it.
  auto iter = browser_states_.find(path);
  if (iter != browser_states_.end()) {
    DCHECK(iter->second.get());
    return iter->second.get();
  }

  scoped_ptr<ChromeBrowserStateImpl> browser_state_impl(
      new ChromeBrowserStateImpl(path));
  DCHECK(!browser_state_impl->IsOffTheRecord());

  std::pair<ChromeBrowserStateImplPathMap::iterator, bool> insert_result =
      browser_states_.insert(
          std::make_pair(path, std::move(browser_state_impl)));
  DCHECK(insert_result.second);
  DCHECK(insert_result.first != browser_states_.end());

  DoFinalInit(insert_result.first->second.get());
  return insert_result.first->second.get();
}

base::FilePath ChromeBrowserStateManagerImpl::GetLastUsedBrowserStateDir(
    const base::FilePath& user_data_dir) {
  PrefService* local_state = GetApplicationContext()->GetLocalState();
  DCHECK(local_state);
  std::string last_used_browser_state_name =
      local_state->GetString(prefs::kBrowserStateLastUsed);
  if (last_used_browser_state_name.empty())
    last_used_browser_state_name = kIOSChromeInitialBrowserState;
  return user_data_dir.AppendASCII(last_used_browser_state_name);
}

BrowserStateInfoCache*
ChromeBrowserStateManagerImpl::GetBrowserStateInfoCache() {
  if (!browser_state_info_cache_) {
    browser_state_info_cache_.reset(new BrowserStateInfoCache(
        GetApplicationContext()->GetLocalState(), GetUserDataDir()));
  }
  return browser_state_info_cache_.get();
}

std::vector<ios::ChromeBrowserState*>
ChromeBrowserStateManagerImpl::GetLoadedBrowserStates() {
  std::vector<ios::ChromeBrowserState*> loaded_browser_states;
  for (const auto& pair : browser_states_)
    loaded_browser_states.push_back(pair.second.get());
  return loaded_browser_states;
}

void ChromeBrowserStateManagerImpl::DoFinalInit(
    ios::ChromeBrowserState* browser_state) {
  DoFinalInitForServices(browser_state);
  AddBrowserStateToCache(browser_state);

  // Log the browser state size after a reasonable startup delay.
  base::FilePath path =
      browser_state->GetOriginalChromeBrowserState()->GetStatePath();
  web::WebThread::PostDelayedTask(web::WebThread::FILE, FROM_HERE,
                                  base::Bind(&BrowserStateSizeTask, path),
                                  base::TimeDelta::FromSeconds(112));

  LogNumberOfBrowserStates(
      GetApplicationContext()->GetChromeBrowserStateManager());
}

void ChromeBrowserStateManagerImpl::DoFinalInitForServices(
    ios::ChromeBrowserState* browser_state) {
  // Activate data reduction proxy. This creates a request context and makes a
  // URL request to check if the data reduction proxy server is reachable.
  IOSChromeDataReductionProxySettingsFactory::GetForBrowserState(browser_state)
      ->MaybeActivateDataReductionProxy(true);
  ios::GaiaCookieManagerServiceFactory::GetForBrowserState(browser_state)
      ->Init();
  ios::AccountConsistencyServiceFactory::GetForBrowserState(browser_state);
  invalidation::ProfileInvalidationProvider* invalidation_provider =
      IOSChromeProfileInvalidationProviderFactory::GetForBrowserState(
          browser_state);
  invalidation::InvalidationService* invalidation_service =
      invalidation_provider ? invalidation_provider->GetInvalidationService()
                            : nullptr;
  ios::AccountFetcherServiceFactory::GetForBrowserState(browser_state)
      ->SetupInvalidationsOnProfileLoad(invalidation_service);
  ios::AccountReconcilorFactory::GetForBrowserState(browser_state);

  // This service is responsible for migration of the legacy password manager
  // preference which controls behaviour of Chrome to the new preference which
  // controls password management behaviour on Chrome and Android. After
  // migration will be performed for all users it's planned to remove the
  // migration code, rough time estimates are Q1 2016.
  IOSChromePasswordManagerSettingMigratorServiceFactory::GetForBrowserState(
      browser_state)
      ->InitializeMigration(
          IOSChromeProfileSyncServiceFactory::GetForBrowserState(
              browser_state));
}

void ChromeBrowserStateManagerImpl::AddBrowserStateToCache(
    ios::ChromeBrowserState* browser_state) {
  DCHECK(!browser_state->IsOffTheRecord());
  BrowserStateInfoCache* cache = GetBrowserStateInfoCache();
  if (browser_state->GetStatePath().DirName() != cache->GetUserDataDir())
    return;

  SigninManagerBase* signin_manager =
      ios::SigninManagerFactory::GetForBrowserState(browser_state);
  AccountTrackerService* account_tracker =
      ios::AccountTrackerServiceFactory::GetForBrowserState(browser_state);
  AccountInfo account_info = account_tracker->GetAccountInfo(
      signin_manager->GetAuthenticatedAccountId());
  base::string16 username = base::UTF8ToUTF16(account_info.email);

  size_t browser_state_index =
      cache->GetIndexOfBrowserStateWithPath(browser_state->GetStatePath());
  if (browser_state_index != std::string::npos) {
    // The BrowserStateInfoCache's info must match the Signin Manager.
    cache->SetAuthInfoOfBrowserStateAtIndex(browser_state_index,
                                            account_info.gaia, username);
    return;
  }
  cache->AddBrowserState(browser_state->GetStatePath(), account_info.gaia,
                         username);
}

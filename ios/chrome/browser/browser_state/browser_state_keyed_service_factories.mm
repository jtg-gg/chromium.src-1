// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/browser_state/browser_state_keyed_service_factories.h"

#include "ios/chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "ios/chrome/browser/autocomplete/in_memory_url_index_factory.h"
#include "ios/chrome/browser/autocomplete/shortcuts_backend_factory.h"
#include "ios/chrome/browser/autofill/personal_data_manager_factory.h"
#include "ios/chrome/browser/bookmarks/bookmark_model_factory.h"
#include "ios/chrome/browser/bookmarks/startup_task_runner_service_factory.h"
#include "ios/chrome/browser/content_settings/cookie_settings_factory.h"
#include "ios/chrome/browser/data_reduction_proxy/ios_chrome_data_reduction_proxy_settings_factory.h"
#include "ios/chrome/browser/dom_distiller/dom_distiller_service_factory.h"
#include "ios/chrome/browser/favicon/favicon_service_factory.h"
#include "ios/chrome/browser/favicon/ios_chrome_favicon_loader_factory.h"
#include "ios/chrome/browser/favicon/ios_chrome_large_icon_cache_factory.h"
#include "ios/chrome/browser/favicon/ios_chrome_large_icon_service_factory.h"
#include "ios/chrome/browser/google/google_url_tracker_factory.h"
#include "ios/chrome/browser/history/history_service_factory.h"
#include "ios/chrome/browser/history/top_sites_factory.h"
#include "ios/chrome/browser/history/web_history_service_factory.h"
#include "ios/chrome/browser/invalidation/ios_chrome_profile_invalidation_provider_factory.h"
#include "ios/chrome/browser/ntp_snippets/ios_chrome_ntp_snippets_service_factory.h"
#include "ios/chrome/browser/passwords/ios_chrome_password_manager_setting_migrator_service_factory.h"
#include "ios/chrome/browser/passwords/ios_chrome_password_store_factory.h"
#include "ios/chrome/browser/search_engines/template_url_service_factory.h"
#include "ios/chrome/browser/services/gcm/ios_chrome_gcm_profile_service_factory.h"
#include "ios/chrome/browser/signin/about_signin_internals_factory.h"
#include "ios/chrome/browser/signin/account_consistency_service_factory.h"
#include "ios/chrome/browser/signin/account_fetcher_service_factory.h"
#include "ios/chrome/browser/signin/account_reconcilor_factory.h"
#include "ios/chrome/browser/signin/account_tracker_service_factory.h"
#include "ios/chrome/browser/signin/gaia_cookie_manager_service_factory.h"
#include "ios/chrome/browser/signin/oauth2_token_service_factory.h"
#include "ios/chrome/browser/signin/signin_client_factory.h"
#include "ios/chrome/browser/signin/signin_error_controller_factory.h"
#include "ios/chrome/browser/signin/signin_manager_factory.h"
#include "ios/chrome/browser/suggestions/suggestions_service_factory.h"
#include "ios/chrome/browser/sync/sync_setup_service_factory.h"
#include "ios/chrome/browser/translate/translate_accept_languages_factory.h"
#include "ios/chrome/browser/undo/bookmark_undo_service_factory.h"
#include "ios/chrome/browser/web_data_service_factory.h"

// This method gets the instance of each ServiceFactory. We do this so that
// each ServiceFactory initializes itself and registers its dependencies with
// the global PreferenceDependencyManager. We need to have a complete
// dependency graph when we create a browser state so we can dispatch the
// browser state creation message to the services that want to create their
// services at browser state creation time.
//
// TODO(erg): This needs to be something else. I don't think putting every
// FooServiceFactory here will scale or is desirable long term.
void EnsureBrowserStateKeyedServiceFactoriesBuilt() {
  dom_distiller::DomDistillerServiceFactory::GetInstance();
  ios::AboutSigninInternalsFactory::GetInstance();
  ios::AccountConsistencyServiceFactory::GetInstance();
  ios::AccountFetcherServiceFactory::GetInstance();
  ios::AccountReconcilorFactory::GetInstance();
  ios::AccountTrackerServiceFactory::GetInstance();
  ios::AutocompleteClassifierFactory::GetInstance();
  ios::BookmarkModelFactory::GetInstance();
  ios::BookmarkUndoServiceFactory::GetInstance();
  ios::CookieSettingsFactory::GetInstance();
  ios::FaviconServiceFactory::GetInstance();
  ios::GaiaCookieManagerServiceFactory::GetInstance();
  ios::GoogleURLTrackerFactory::GetInstance();
  ios::HistoryServiceFactory::GetInstance();
  ios::InMemoryURLIndexFactory::GetInstance();
  ios::ShortcutsBackendFactory::GetInstance();
  ios::SigninErrorControllerFactory::GetInstance();
  ios::SigninManagerFactory::GetInstance();
  ios::StartupTaskRunnerServiceFactory::GetInstance();
  ios::TemplateURLServiceFactory::GetInstance();
  ios::TopSitesFactory::GetInstance();
  ios::WebDataServiceFactory::GetInstance();
  ios::WebHistoryServiceFactory::GetInstance();
  IOSChromeDataReductionProxySettingsFactory::GetInstance();
  IOSChromeGCMProfileServiceFactory::GetInstance();
  IOSChromeLargeIconCacheFactory::GetInstance();
  IOSChromeLargeIconServiceFactory::GetInstance();
  IOSChromeFaviconLoaderFactory::GetInstance();
  IOSChromeNTPSnippetsServiceFactory::GetInstance();
  IOSChromePasswordManagerSettingMigratorServiceFactory::GetInstance();
  IOSChromePasswordStoreFactory::GetInstance();
  IOSChromeProfileInvalidationProviderFactory::GetInstance();
  OAuth2TokenServiceFactory::GetInstance();
  PersonalDataManagerFactory::GetInstance();
  SigninClientFactory::GetInstance();
  suggestions::SuggestionsServiceFactory::GetInstance();
  SyncSetupServiceFactory::GetInstance();
  TranslateAcceptLanguagesFactory::GetInstance();
}

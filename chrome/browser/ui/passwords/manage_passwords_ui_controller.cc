// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"

#include <utility>

#include "base/auto_reset.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/browsing_data/browsing_data_helper.h"
#include "chrome/browser/password_manager/chrome_password_manager_client.h"
#include "chrome/browser/password_manager/password_store_factory.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/passwords/manage_passwords_icon_view.h"
#include "chrome/browser/ui/passwords/password_dialog_controller_impl.h"
#include "chrome/browser/ui/passwords/password_dialog_prompts.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/common/url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/password_manager/core/browser/browser_save_password_progress_logger.h"
#include "components/password_manager/core/browser/password_bubble_experiment.h"
#include "components/password_manager/core/browser/password_form_manager.h"
#include "components/password_manager/core/browser/password_manager_constants.h"
#include "components/password_manager/core/browser/statistics_table.h"
#include "components/password_manager/core/common/credential_manager_types.h"
#include "content/public/browser/navigation_details.h"
#include "ui/base/l10n/l10n_util.h"

using autofill::PasswordFormMap;
using password_manager::PasswordFormManager;

namespace {

password_manager::PasswordStore* GetPasswordStore(
    content::WebContents* web_contents) {
  return PasswordStoreFactory::GetForProfile(
             Profile::FromBrowserContext(web_contents->GetBrowserContext()),
             ServiceAccessType::EXPLICIT_ACCESS).get();
}

std::vector<scoped_ptr<autofill::PasswordForm>> CopyFormVector(
    const ScopedVector<autofill::PasswordForm>& forms) {
  std::vector<scoped_ptr<autofill::PasswordForm>> result(forms.size());
  for (size_t i = 0; i < forms.size(); ++i)
    result[i].reset(new autofill::PasswordForm(*forms[i]));
  return result;
}

}  // namespace

DEFINE_WEB_CONTENTS_USER_DATA_KEY(ManagePasswordsUIController);

ManagePasswordsUIController::ManagePasswordsUIController(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      bubble_status_(NOT_SHOWN) {
  passwords_data_.set_client(
      ChromePasswordManagerClient::FromWebContents(web_contents));
  password_manager::PasswordStore* password_store =
      GetPasswordStore(web_contents);
  if (password_store)
    password_store->AddObserver(this);
}

ManagePasswordsUIController::~ManagePasswordsUIController() {}

void ManagePasswordsUIController::OnPasswordSubmitted(
    scoped_ptr<PasswordFormManager> form_manager) {
  bool show_bubble = !form_manager->IsBlacklisted();
  DestroyAccountChooser();
  passwords_data_.OnPendingPassword(std::move(form_manager));
  if (show_bubble) {
    password_manager::InteractionsStats* stats = GetCurrentInteractionStats();
    const int show_threshold =
        password_bubble_experiment::GetSmartBubbleDismissalThreshold();
    if (stats && show_threshold > 0 && stats->dismissal_count >= show_threshold)
      show_bubble = false;
  }
  if (show_bubble)
    bubble_status_ = SHOULD_POP_UP;
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::OnUpdatePasswordSubmitted(
    scoped_ptr<PasswordFormManager> form_manager) {
  DestroyAccountChooser();
  passwords_data_.OnUpdatePassword(std::move(form_manager));
  bubble_status_ = SHOULD_POP_UP;
  UpdateBubbleAndIconVisibility();
}

bool ManagePasswordsUIController::OnChooseCredentials(
    ScopedVector<autofill::PasswordForm> local_credentials,
    ScopedVector<autofill::PasswordForm> federated_credentials,
    const GURL& origin,
    base::Callback<void(const password_manager::CredentialInfo&)> callback) {
  DCHECK(!local_credentials.empty() || !federated_credentials.empty());
  PasswordDialogController::FormsVector locals =
      CopyFormVector(local_credentials);
  PasswordDialogController::FormsVector federations =
      CopyFormVector(federated_credentials);
  passwords_data_.OnRequestCredentials(
      std::move(local_credentials), std::move(federated_credentials), origin);
  passwords_data_.set_credentials_callback(callback);
  dialog_controller_.reset(new PasswordDialogControllerImpl(
      Profile::FromBrowserContext(web_contents()->GetBrowserContext()),
      this));
  dialog_controller_->ShowAccountChooser(
      CreateAccountChooser(dialog_controller_.get()),
      std::move(locals), std::move(federations));
  UpdateBubbleAndIconVisibility();
  return true;
}

void ManagePasswordsUIController::OnAutoSignin(
    ScopedVector<autofill::PasswordForm> local_forms) {
  DCHECK(!local_forms.empty());
  DestroyAccountChooser();
  passwords_data_.OnAutoSignin(std::move(local_forms));
  bubble_status_ = SHOULD_POP_UP;
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::OnPromptEnableAutoSignin() {
  // Both the account chooser and the previous prompt shouldn't be closed.
  if (dialog_controller_)
    return;
  dialog_controller_.reset(new PasswordDialogControllerImpl(
      Profile::FromBrowserContext(web_contents()->GetBrowserContext()),
      this));
  dialog_controller_->ShowAutosigninPrompt(
      CreateAutoSigninPrompt(dialog_controller_.get()));
}

void ManagePasswordsUIController::OnAutomaticPasswordSave(
    scoped_ptr<PasswordFormManager> form_manager) {
  DestroyAccountChooser();
  passwords_data_.OnAutomaticPasswordSave(std::move(form_manager));
  bubble_status_ = SHOULD_POP_UP;
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::OnPasswordAutofilled(
    const autofill::PasswordFormMap& password_form_map,
    const GURL& origin,
    const std::vector<scoped_ptr<autofill::PasswordForm>>* federated_matches) {
  // If we fill a form while a dialog is open, then skip the state change; we
  // have
  // the information we need, and the dialog will change its own state once the
  // interaction is complete.
  if (passwords_data_.state() !=
          password_manager::ui::AUTO_SIGNIN_STATE &&
      passwords_data_.state() !=
          password_manager::ui::CREDENTIAL_REQUEST_STATE) {
    passwords_data_.OnPasswordAutofilled(password_form_map, origin,
                                         federated_matches);
    UpdateBubbleAndIconVisibility();
  }
}

void ManagePasswordsUIController::OnLoginsChanged(
    const password_manager::PasswordStoreChangeList& changes) {
  password_manager::ui::State current_state = GetState();
  passwords_data_.ProcessLoginsChanged(changes);
  if (current_state != GetState())
    UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::UpdateIconAndBubbleState(
    ManagePasswordsIconView* icon) {
  if (bubble_status_ == SHOULD_POP_UP) {
    DCHECK(!dialog_controller_);
    // We must display the icon before showing the bubble, as the bubble would
    // be otherwise unanchored.
    icon->SetState(GetState());
    ShowBubbleWithoutUserInteraction();
    // If the bubble appeared then the status is updated in OnBubbleShown().
    if (bubble_status_ == SHOULD_POP_UP)
      bubble_status_ = NOT_SHOWN;
  } else {
    password_manager::ui::State state = GetState();
    // The dialog should hide the icon.
    if (dialog_controller_ &&
        state == password_manager::ui::CREDENTIAL_REQUEST_STATE)
      state = password_manager::ui::INACTIVE_STATE;
    icon->SetState(state);
  }
}

const GURL& ManagePasswordsUIController::GetOrigin() const {
  return passwords_data_.origin();
}

password_manager::ui::State ManagePasswordsUIController::GetState() const {
  return passwords_data_.state();
}

const autofill::PasswordForm& ManagePasswordsUIController::
    GetPendingPassword() const {
  if (GetState() == password_manager::ui::AUTO_SIGNIN_STATE)
    return *GetCurrentForms()[0];

  DCHECK(GetState() == password_manager::ui::PENDING_PASSWORD_STATE ||
         GetState() == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
         GetState() == password_manager::ui::CONFIRMATION_STATE)
      << GetState();
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  return form_manager->pending_credentials();
}

bool ManagePasswordsUIController::IsPasswordOverridden() const {
  const password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  return form_manager ? form_manager->password_overridden() : false;
}

const std::vector<const autofill::PasswordForm*>&
ManagePasswordsUIController::GetCurrentForms() const {
  return passwords_data_.GetCurrentForms();
}

const std::vector<const autofill::PasswordForm*>&
ManagePasswordsUIController::GetFederatedForms() const {
  return passwords_data_.federated_credentials_forms();
}

password_manager::InteractionsStats*
ManagePasswordsUIController::GetCurrentInteractionStats() const {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_STATE, GetState());
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  return password_manager::FindStatsByUsername(
      form_manager->interactions_stats(),
      form_manager->pending_credentials().username_value);
}

void ManagePasswordsUIController::OnBubbleShown() {
  bubble_status_ = SHOWN;
}

void ManagePasswordsUIController::OnBubbleHidden() {
  bubble_status_ = NOT_SHOWN;
  if (GetState() == password_manager::ui::CONFIRMATION_STATE ||
      GetState() == password_manager::ui::AUTO_SIGNIN_STATE) {
    passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
    UpdateBubbleAndIconVisibility();
  }
}

void ManagePasswordsUIController::OnNoInteractionOnUpdate() {
  if (GetState() != password_manager::ui::PENDING_PASSWORD_UPDATE_STATE) {
    // Do nothing if the state was changed. It can happen for example when the
    // update bubble is active and a page navigation happens.
    return;
  }
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  DCHECK(form_manager);
  form_manager->OnNoInteractionOnUpdate();
}

void ManagePasswordsUIController::OnNopeUpdateClicked() {
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  DCHECK(form_manager);
  form_manager->OnNopeUpdateClicked();
}

void ManagePasswordsUIController::NeverSavePassword() {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_STATE, GetState());
  NeverSavePasswordInternal();
  // The state stays the same.
}

void ManagePasswordsUIController::SavePassword() {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_STATE, GetState());
  SavePasswordInternal();
  passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::UpdatePassword(
    const autofill::PasswordForm& password_form) {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_UPDATE_STATE, GetState());
  UpdatePasswordInternal(password_form);
  passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::ChooseCredential(
    autofill::PasswordForm form,
    password_manager::CredentialType credential_type) {
  DCHECK(dialog_controller_);
  dialog_controller_.reset();
  passwords_data_.ChooseCredential(form, credential_type);
  passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::NavigateToExternalPasswordManager() {
  chrome::NavigateParams params(
      chrome::FindBrowserWithWebContents(web_contents()),
      GURL(password_manager::kPasswordManagerAccountDashboardURL),
      ui::PAGE_TRANSITION_LINK);
  params.disposition = NEW_FOREGROUND_TAB;
  chrome::Navigate(&params);
}

void ManagePasswordsUIController::NavigateToSmartLockHelpPage() {
  chrome::NavigateParams params(
      chrome::FindBrowserWithWebContents(web_contents()),
      GURL(chrome::kSmartLockHelpPage), ui::PAGE_TRANSITION_LINK);
  params.disposition = NEW_FOREGROUND_TAB;
  chrome::Navigate(&params);
}

void ManagePasswordsUIController::NavigateToPasswordManagerSettingsPage() {
  chrome::ShowSettingsSubPage(
      chrome::FindBrowserWithWebContents(web_contents()),
      chrome::kPasswordManagerSubPage);
}

void ManagePasswordsUIController::OnDialogHidden() {
  dialog_controller_.reset();
  if (GetState() == password_manager::ui::CREDENTIAL_REQUEST_STATE) {
    passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
    UpdateBubbleAndIconVisibility();
  }
}

void ManagePasswordsUIController::SavePasswordInternal() {
  password_manager::PasswordStore* password_store =
      GetPasswordStore(web_contents());
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  for (const autofill::PasswordForm* form :
       form_manager->blacklisted_matches()) {
    password_store->RemoveLogin(*form);
  }

  form_manager->Save();
}

void ManagePasswordsUIController::UpdatePasswordInternal(
    const autofill::PasswordForm& password_form) {
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  form_manager->Update(password_form);
}

void ManagePasswordsUIController::NeverSavePasswordInternal() {
  password_manager::PasswordFormManager* form_manager =
      passwords_data_.form_manager();
  DCHECK(form_manager);
  form_manager->PermanentlyBlacklist();
}

void ManagePasswordsUIController::UpdateBubbleAndIconVisibility() {
  // If we're not on a "webby" URL (e.g. "chrome://sign-in"), we shouldn't
  // display either the bubble or the icon.
  if (!BrowsingDataHelper::IsWebScheme(
          web_contents()->GetLastCommittedURL().scheme())) {
    passwords_data_.OnInactive();
  }

  Browser* browser = chrome::FindBrowserWithWebContents(web_contents());
  if (!browser)
    return;
  LocationBar* location_bar = browser->window()->GetLocationBar();
  DCHECK(location_bar);
  location_bar->UpdateManagePasswordsIconAndBubble();
}

AccountChooserPrompt* ManagePasswordsUIController::CreateAccountChooser(
    PasswordDialogController* controller) {
  return CreateAccountChooserPromptView(controller, web_contents());
}

AutoSigninFirstRunPrompt* ManagePasswordsUIController::CreateAutoSigninPrompt(
    PasswordDialogController* controller) {
  return CreateAutoSigninPromptView(controller, web_contents());
}

void ManagePasswordsUIController::DidNavigateMainFrame(
    const content::LoadCommittedDetails& details,
    const content::FrameNavigateParams& params) {
  // Don't react to in-page (fragment) navigations.
  if (details.is_in_page)
    return;

  // It is possible that the user was not able to interact with the password
  // bubble.
  if (bubble_status_ == SHOWN)
    return;

  // Otherwise, reset the password manager.
  DestroyAccountChooser();
  passwords_data_.OnInactive();
  UpdateBubbleAndIconVisibility();
}

void ManagePasswordsUIController::WasHidden() {
  if (TabDialogs::FromWebContents(web_contents()))
    TabDialogs::FromWebContents(web_contents())->HideManagePasswordsBubble();
}

void ManagePasswordsUIController::ShowBubbleWithoutUserInteraction() {
  DCHECK(IsAutomaticallyOpeningBubble());
  Browser* browser = chrome::FindBrowserWithWebContents(web_contents());
  if (!browser || browser->toolbar_model()->input_in_progress())
    return;

  CommandUpdater* updater = browser->command_controller()->command_updater();
  updater->ExecuteCommand(IDC_MANAGE_PASSWORDS_FOR_PAGE);
}

void ManagePasswordsUIController::DestroyAccountChooser() {
  if (dialog_controller_ && dialog_controller_->IsShowingAccountChooser()) {
    dialog_controller_.reset();
    passwords_data_.TransitionToState(password_manager::ui::MANAGE_STATE);
  }
}

void ManagePasswordsUIController::WebContentsDestroyed() {
  password_manager::PasswordStore* password_store =
      GetPasswordStore(web_contents());
  if (password_store)
    password_store->RemoveObserver(this);
}

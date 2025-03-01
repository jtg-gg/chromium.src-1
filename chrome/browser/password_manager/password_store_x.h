// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PASSWORD_MANAGER_PASSWORD_STORE_X_H_
#define CHROME_BROWSER_PASSWORD_MANAGER_PASSWORD_STORE_X_H_

#include <stddef.h>

#include <vector>

#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/scoped_vector.h"
#include "base/time/time.h"
#include "components/password_manager/core/browser/password_store_default.h"

class PrefService;

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace password_manager {
class LoginDatabase;
}

// PasswordStoreX is used on Linux and other non-Windows, non-Mac OS X
// operating systems. It uses a "native backend" to actually store the password
// data when such a backend is available, and otherwise falls back to using the
// login database like PasswordStoreDefault. It also handles automatically
// migrating password data to a native backend from the login database.
//
// There are currently native backends for GNOME Keyring and KWallet.
class PasswordStoreX : public password_manager::PasswordStoreDefault {
 public:
  // NativeBackends more or less implement the PaswordStore interface, but
  // with return values rather than implicit consumer notification.
  class NativeBackend {
   public:
    virtual ~NativeBackend() {}

    virtual bool Init() = 0;

    virtual password_manager::PasswordStoreChangeList AddLogin(
        const autofill::PasswordForm& form) = 0;
    // Updates |form| and appends the changes to |changes|. |changes| shouldn't
    // be null. Returns false iff the operation failed due to a system backend
    // error.
    virtual bool UpdateLogin(
        const autofill::PasswordForm& form,
        password_manager::PasswordStoreChangeList* changes) = 0;
    // Removes |form| and appends the changes to |changes|. |changes| shouldn't
    // be null. Returns false iff the operation failed due to a system backend
    // error.
    virtual bool RemoveLogin(
        const autofill::PasswordForm& form,
        password_manager::PasswordStoreChangeList* changes) = 0;

    // Removes all logins created/synced from |delete_begin| onwards (inclusive)
    // and before |delete_end|. You may use a null Time value to do an unbounded
    // delete in either direction.
    virtual bool RemoveLoginsCreatedBetween(
        base::Time delete_begin,
        base::Time delete_end,
        password_manager::PasswordStoreChangeList* changes) = 0;
    virtual bool RemoveLoginsSyncedBetween(
        base::Time delete_begin,
        base::Time delete_end,
        password_manager::PasswordStoreChangeList* changes) = 0;

    // Sets the 'skip_zero_click' flag to 'true' for all logins in the database.
    virtual bool DisableAutoSignInForAllLogins(
        password_manager::PasswordStoreChangeList* changes) = 0;

    // The three methods below overwrite |forms| with all stored credentials
    // matching |form|, all stored non-blacklisted credentials, and all stored
    // blacklisted credentials, respectively. On success, they return true.
    virtual bool GetLogins(const autofill::PasswordForm& form,
                           ScopedVector<autofill::PasswordForm>* forms)
        WARN_UNUSED_RESULT = 0;
    virtual bool GetAutofillableLogins(
        ScopedVector<autofill::PasswordForm>* forms) WARN_UNUSED_RESULT = 0;
    virtual bool GetBlacklistLogins(ScopedVector<autofill::PasswordForm>* forms)
        WARN_UNUSED_RESULT = 0;
    virtual bool GetAllLogins(ScopedVector<autofill::PasswordForm>* forms)
        WARN_UNUSED_RESULT = 0;
  };

  // Takes ownership of |login_db| and |backend|. |backend| may be NULL in which
  // case this PasswordStoreX will act the same as PasswordStoreDefault.
  PasswordStoreX(scoped_refptr<base::SingleThreadTaskRunner> main_thread_runner,
                 scoped_refptr<base::SingleThreadTaskRunner> db_thread_runner,
                 scoped_ptr<password_manager::LoginDatabase> login_db,
                 NativeBackend* backend);

 private:
  friend class PasswordStoreXTest;

  ~PasswordStoreX() override;

  // Implements PasswordStore interface.
  password_manager::PasswordStoreChangeList AddLoginImpl(
      const autofill::PasswordForm& form) override;
  password_manager::PasswordStoreChangeList UpdateLoginImpl(
      const autofill::PasswordForm& form) override;
  password_manager::PasswordStoreChangeList RemoveLoginImpl(
      const autofill::PasswordForm& form) override;
  password_manager::PasswordStoreChangeList RemoveLoginsByOriginAndTimeImpl(
      const url::Origin& origin,
      base::Time delete_begin,
      base::Time delete_end) override;
  password_manager::PasswordStoreChangeList RemoveLoginsCreatedBetweenImpl(
      base::Time delete_begin,
      base::Time delete_end) override;
  password_manager::PasswordStoreChangeList RemoveLoginsSyncedBetweenImpl(
      base::Time delete_begin,
      base::Time delete_end) override;
  password_manager::PasswordStoreChangeList DisableAutoSignInForAllLoginsImpl()
      override;
  ScopedVector<autofill::PasswordForm> FillMatchingLogins(
      const autofill::PasswordForm& form) override;
  bool FillAutofillableLogins(
      ScopedVector<autofill::PasswordForm>* forms) override;
  bool FillBlacklistLogins(
      ScopedVector<autofill::PasswordForm>* forms) override;

  // Check to see whether migration is necessary, and perform it if so.
  void CheckMigration();

  // Return true if we should try using the native backend.
  bool use_native_backend() { return !!backend_.get(); }

  // Return true if we can fall back on the default store, warning the first
  // time we call it when falling back is necessary. See |allow_fallback_|.
  bool allow_default_store();

  // Synchronously migrates all the passwords stored in the login database to
  // the native backend. If successful, the login database will be left with no
  // stored passwords, and the number of passwords migrated will be returned.
  // (This might be 0 if migration was not necessary.) Returns < 0 on failure.
  ssize_t MigrateLogins();

  // The native backend in use, or NULL if none.
  scoped_ptr<NativeBackend> backend_;
  // Whether we have already attempted migration to the native store.
  bool migration_checked_;
  // Whether we should allow falling back to the default store. If there is
  // nothing to migrate, then the first attempt to use the native store will
  // be the first time we try to use it and we should allow falling back. If
  // we have migrated successfully, then we do not allow falling back.
  bool allow_fallback_;

  DISALLOW_COPY_AND_ASSIGN(PasswordStoreX);
};

#endif  // CHROME_BROWSER_PASSWORD_MANAGER_PASSWORD_STORE_X_H_

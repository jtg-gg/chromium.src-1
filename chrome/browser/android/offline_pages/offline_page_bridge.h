// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_OFFLINE_PAGES_OFFLINE_PAGE_BRIDGE_H_
#define CHROME_BROWSER_ANDROID_OFFLINE_PAGES_OFFLINE_PAGE_BRIDGE_H_

#include <stdint.h>

#include "base/android/jni_android.h"
#include "base/android/jni_weak_ref.h"
#include "base/macros.h"
#include "components/offline_pages/offline_page_model.h"

namespace content {
class BrowserContext;
}

namespace offline_pages {
namespace android {

/**
 * Bridge between C++ and Java for exposing native implementation of offline
 * pages model in managed code.
 */
class OfflinePageBridge : public OfflinePageModel::Observer {
 public:
  OfflinePageBridge(JNIEnv* env,
                    jobject obj,
                    content::BrowserContext* browser_context);
  void Destroy(JNIEnv*, const base::android::JavaParamRef<jobject>&);

  // OfflinePageModel::Observer implementation.
  void OfflinePageModelLoaded(OfflinePageModel* model) override;
  void OfflinePageModelChanged(OfflinePageModel* model) override;
  void OfflinePageDeleted(int64_t bookmark_id) override;

  void GetAllPages(JNIEnv* env,
                   const base::android::JavaParamRef<jobject>& obj,
                   const base::android::JavaParamRef<jobject>& j_result_obj);
  void GetPagesToCleanUp(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jobject>& j_result_obj);

  base::android::ScopedJavaLocalRef<jobject> GetPageByBookmarkId(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      jlong bookmark_id);

  base::android::ScopedJavaLocalRef<jobject> GetPageByOnlineURL(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jstring>& online_url);

  base::android::ScopedJavaLocalRef<jobject> GetPageByOfflineUrl(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jstring>& j_offline_url);

  void SavePage(JNIEnv* env,
                const base::android::JavaParamRef<jobject>& obj,
                const base::android::JavaParamRef<jobject>& j_callback_obj,
                const base::android::JavaParamRef<jobject>& j_web_contents,
                jlong bookmark_id);

  void MarkPageAccessed(JNIEnv* env,
                        const base::android::JavaParamRef<jobject>& obj,
                        jlong bookmark_id);

  void DeletePage(JNIEnv* env,
                  const base::android::JavaParamRef<jobject>& obj,
                  const base::android::JavaParamRef<jobject>& j_callback_obj,
                  jlong bookmark_id);

  void DeletePages(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jobject>& j_callback_obj,
      const base::android::JavaParamRef<jlongArray>& bookmark_ids_array);

  void CheckMetadataConsistency(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  base::android::ScopedJavaLocalRef<jstring> GetOfflineUrlForOnlineUrl(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jstring>& j_online_url);

 private:
  void NotifyIfDoneLoading() const;

  base::android::ScopedJavaLocalRef<jobject> CreateOfflinePageItem(
      JNIEnv* env,
      const OfflinePageItem& offline_page) const;

  JavaObjectWeakGlobalRef weak_java_ref_;
  // Not owned.
  content::BrowserContext* browser_context_;
  // Not owned.
  OfflinePageModel* offline_page_model_;

  DISALLOW_COPY_AND_ASSIGN(OfflinePageBridge);
};

bool RegisterOfflinePageBridge(JNIEnv* env);

}  // namespace android
}  // namespace offline_pages

#endif  // CHROME_BROWSER_ANDROID_OFFLINE_PAGES_OFFLINE_PAGE_BRIDGE_H_


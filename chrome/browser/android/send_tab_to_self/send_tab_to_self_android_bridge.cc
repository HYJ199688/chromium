// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_android.h"
#include "chrome/browser/send_tab_to_self/send_tab_to_self_util.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "components/send_tab_to_self/send_tab_to_self_entry.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "content/public/browser/web_contents.h"
#include "jni/SendTabToSelfAndroidBridge_jni.h"
#include "jni/SendTabToSelfEntry_jni.h"
#include "url/gurl.h"

using base::android::AttachCurrentThread;
using base::android::ConvertUTF8ToJavaString;
using base::android::JavaParamRef;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;

// The delegate to fetch SendTabToSelf information and persist new
// SendTabToSelf entries. The functions are called by the SendTabToSelf Java
// counterpart.
namespace send_tab_to_self {

namespace {

ScopedJavaLocalRef<jobject> CreateJavaSendTabToSelfEntry(
    JNIEnv* env,
    const SendTabToSelfEntry* entry) {
  return Java_SendTabToSelfEntry_createSendTabToSelfEntry(
      env, ConvertUTF8ToJavaString(env, entry->GetGUID()),
      ConvertUTF8ToJavaString(env, entry->GetURL().spec()),
      ConvertUTF8ToJavaString(env, entry->GetTitle()),
      entry->GetSharedTime().ToJavaTime(),
      entry->GetOriginalNavigationTime().ToJavaTime(),
      ConvertUTF8ToJavaString(env, entry->GetDeviceName()));
}

SendTabToSelfModel* GetModel(const JavaParamRef<jobject>& j_profile) {
  Profile* profile = ProfileAndroid::FromProfileAndroid(j_profile);
  return SendTabToSelfSyncServiceFactory::GetInstance()
      ->GetForProfile(profile)
      ->GetSendTabToSelfModel();
}

}  // namespace

// Populates a list of GUIDs in the model.
static void JNI_SendTabToSelfAndroidBridge_GetAllGuids(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jobject>& j_guid_list_obj) {
  // TODO(tgupta): Check that the model is loaded
  // if (!send_tab_to_self_model_->loaded()())
  //   return;

  SendTabToSelfModel* model = GetModel(j_profile);
  std::vector<std::string> all_ids = model->GetAllGuids();
  for (std::vector<std::string>::iterator it = all_ids.begin();
       it != all_ids.end(); ++it) {
    ScopedJavaLocalRef<jstring> j_guid = ConvertUTF8ToJavaString(env, *it);
    Java_SendTabToSelfAndroidBridge_addToGuidList(env, j_guid_list_obj, j_guid);
  }
}

// Deletes all entries in the model.
void JNI_SendTabToSelfAndroidBridge_DeleteAllEntries(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile) {
  GetModel(j_profile)->DeleteAllEntries();
}

// Adds a new entry with the specified parameters. Returns the persisted
// version which contains additional information such as GUID.
static ScopedJavaLocalRef<jobject> JNI_SendTabToSelfAndroidBridge_AddEntry(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jstring>& j_url,
    const JavaParamRef<jstring>& j_title,
    jlong j_navigation_time) {
  const std::string url = ConvertJavaStringToUTF8(env, j_url);
  const std::string title = ConvertJavaStringToUTF8(env, j_title);
  base::Time navigation_time = base::Time::FromJavaTime(j_navigation_time);

  const SendTabToSelfEntry* persisted_entry =
      GetModel(j_profile)->AddEntry(GURL(url), title, navigation_time);

  if (persisted_entry == nullptr) {
    return nullptr;
  }

  return CreateJavaSendTabToSelfEntry(env, persisted_entry);
}

// Returns the entry associated with a GUID. May return nullptr if none is
// found.
static ScopedJavaLocalRef<jobject>
JNI_SendTabToSelfAndroidBridge_GetEntryByGUID(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jstring>& j_guid) {
  const std::string guid = ConvertJavaStringToUTF8(env, j_guid);
  const SendTabToSelfEntry* found_entry =
      GetModel(j_profile)->GetEntryByGUID(guid);

  if (found_entry == nullptr) {
    return nullptr;
  }

  return CreateJavaSendTabToSelfEntry(env, found_entry);
}

// Deletes the entry associated with the passed in GUID.
static void JNI_SendTabToSelfAndroidBridge_DeleteEntry(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jstring>& j_guid) {
  const std::string guid = ConvertJavaStringToUTF8(env, j_guid);
  GetModel(j_profile)->DeleteEntry(guid);
}

// Marks the entry with the associated GUID as dismissed.
static void JNI_SendTabToSelfAndroidBridge_DismissEntry(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jstring>& j_guid) {
  const std::string guid = ConvertJavaStringToUTF8(env, j_guid);
  GetModel(j_profile)->DismissEntry(guid);
}

// Returns whether the feature is available for the specified |profile| and
// |web_contents|.
static jboolean JNI_SendTabToSelfAndroidBridge_IsFeatureAvailable(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jobject>& j_web_contents) {
  Profile* profile = ProfileAndroid::FromProfileAndroid(j_profile);
  content::WebContents* web_contents =
      content::WebContents::FromJavaWebContents(j_web_contents);

  return ShouldOfferFeature(profile, web_contents);
}

}  // namespace send_tab_to_self

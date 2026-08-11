// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROFILE_DRAFT_H_
#define CHROME_BROWSER_MONTI_MONTI_PROFILE_DRAFT_H_

#include <string>
#include <vector>

#include "chrome/browser/monti/monti_fingerprint.h"

namespace monti {

// A platform login credential the New-profile dialog captures when the user
// selects a Profile-type chip. Held in plaintext only while in flight; the
// service persists it OSCrypt-encrypted (see MontiProfileService::SetCredentials)
// and the login-page auto-fill is deferred to a later session.
struct ProfileCredential {
  std::string platform;  // the Profile-type key, e.g. "google" | "fb"
  std::string email;
  std::string password;
};

// The complete New-profile dialog draft. MontiManager::CreateProfileFull persists
// every field in one shot; mirrors monti::mojom::ProfileDraft.
struct ProfileDraft {
  std::string name;
  std::string proxy_id;
  std::string folder_id;
  std::vector<std::string> tags;
  std::string status;
  std::string color;
  std::string profile_type;
  std::vector<std::string> start_pages;
  std::string cookies;  // pasted/uploaded draft blob; injection deferred
  Fingerprint fingerprint;
  std::vector<ProfileCredential> credentials;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROFILE_DRAFT_H_

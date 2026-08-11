// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROFILE_ENTRY_H_
#define CHROME_BROWSER_MONTI_MONTI_PROFILE_ENTRY_H_

#include <string>
#include <vector>

namespace monti {

// Lightweight registry record for a single Monti profile. Mirrors the subset
// of per-profile metadata needed to render the profiles list without loading
// every Chrome profile. Owned and persisted by ProfileStore.
struct MontiProfileEntry {
  std::string id;                 // = the Chrome profile directory basename
  std::string name;               // display name
  std::string profile_dir;        // profile directory basename (== id)
  std::string assigned_proxy_id;  // proxy id or empty
  std::string created_at;         // ISO/local import timestamp, if known
  std::string profile_status;     // user-owned status label

  // Manager metadata (session 13). Additive; defaults are empty so older
  // profiles.json files load unchanged.
  std::vector<std::string> tags;  // free-form labels
  std::string notes;              // free-form note text
  std::string color;              // CSS color for the card swatch (or empty)
  std::string folder_id;          // owning folder id (empty == ungrouped)

  // Fingerprint identity (session 14). UA preset key: "windows" | "macos" |
  // "linux"; empty (or "system") means no override. Additive; older
  // profiles.json files default to no override.
  std::string ua_preset;

  // Full anti-detect fingerprint (session 19), held as a compact JSON string
  // (monti::ToJson/FromJson) so this registry record stays trivially copyable.
  // Empty until the profile is first created or its fingerprint is lazily
  // generated. Additive; older profiles.json files default to empty.
  std::string fingerprint;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROFILE_ENTRY_H_

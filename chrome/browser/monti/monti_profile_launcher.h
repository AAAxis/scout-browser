// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROFILE_LAUNCHER_H_
#define CHROME_BROWSER_MONTI_MONTI_PROFILE_LAUNCHER_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace monti {

struct MontiProfileEntry;

// Creates or updates the per-profile macOS launcher app used for Dolphin-style
// Dock entries. No-ops on non-mac platforms.
void CreateOrUpdateProfileLauncher(const MontiProfileEntry& entry,
                                   std::vector<base::FilePath> extensions,
                                   std::string launch_switches);

// Removes the launcher app for `profile_dir`. No-ops on non-mac platforms.
void RemoveProfileLauncher(const std::string& profile_dir);

// Opens the launcher app for `entry` if present. Returns false on platforms
// without launchers, or when the launcher could not be opened.
bool LaunchProfileLauncher(const MontiProfileEntry& entry);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROFILE_LAUNCHER_H_

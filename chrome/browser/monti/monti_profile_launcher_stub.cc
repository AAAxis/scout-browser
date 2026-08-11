// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_profile_launcher.h"

#include <utility>

#include "chrome/browser/monti/monti_profile_entry.h"

namespace monti {

void CreateOrUpdateProfileLauncher(const MontiProfileEntry& entry,
                                   std::vector<base::FilePath> extensions,
                                   std::string launch_switches) {}

void RemoveProfileLauncher(const std::string& profile_dir) {}

bool LaunchProfileLauncher(const MontiProfileEntry& entry) {
  return false;
}

}  // namespace monti

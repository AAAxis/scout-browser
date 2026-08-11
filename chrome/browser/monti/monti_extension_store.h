// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_EXTENSION_STORE_H_
#define CHROME_BROWSER_MONTI_MONTI_EXTENSION_STORE_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace monti {

struct SharedExtension {
  std::string name;
  base::FilePath path;
};

std::vector<SharedExtension> GetSharedExtensions();
std::vector<base::FilePath> GetSharedExtensionPaths();
std::string GetSharedLaunchSwitches();
std::string ExportSharedExtensionsJson();
void ReplaceSharedExtensionsFromJson(const std::string& json);
void AddSharedExtensionPath(const base::FilePath& path);
void RemoveSharedExtensionPath(const base::FilePath& path);
void SetSharedLaunchSwitches(const std::string& switches);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_EXTENSION_STORE_H_

// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_FOLDER_H_
#define CHROME_BROWSER_MONTI_MONTI_FOLDER_H_

#include <string>

namespace monti {

// A folder used to organize profiles in the chrome://monti manager. Owned and
// persisted by FolderStore. `parent_id` is empty for a top-level folder.
struct Folder {
  std::string id;
  std::string parent_id;
  std::string name;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_FOLDER_H_

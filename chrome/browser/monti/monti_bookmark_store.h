// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_BOOKMARK_STORE_H_
#define CHROME_BROWSER_MONTI_MONTI_BOOKMARK_STORE_H_

#include <string>
#include <vector>

namespace monti {

struct SharedBookmark {
  std::string title;
  std::string url;
  std::string icon;
};

std::vector<SharedBookmark> GetSharedBookmarks();
std::string ExportSharedBookmarksJson();
void ReplaceSharedBookmarksFromJson(const std::string& json);
void AddOrUpdateSharedBookmark(const std::string& title,
                               const std::string& url);
void RemoveSharedBookmark(const std::string& url);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_BOOKMARK_STORE_H_

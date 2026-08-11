// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_bookmark_store.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "chrome/common/chrome_paths.h"
#include "url/gurl.h"

namespace monti {
namespace {

base::FilePath StorePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.AppendASCII("Monti").AppendASCII("bookmarks.json");
}

std::string NormalizeUrl(const std::string& url) {
  GURL parsed(url);
  if (!parsed.is_valid() || !parsed.has_scheme()) {
    parsed = GURL(base::StrCat({"https://", url}));
  }
  return parsed.is_valid() ? parsed.spec() : url;
}

std::string FaviconForUrl(const std::string& url) {
  GURL parsed(url);
  if (!parsed.is_valid() || !parsed.has_host()) {
    return std::string();
  }
  return base::StrCat({parsed.scheme(), "://", parsed.host(), "/favicon.ico"});
}

bool ShouldRefreshIcon(const std::string& icon) {
  return icon.empty() ||
         icon.find("google.com/s2/favicons") != std::string::npos;
}

SharedBookmark DictToBookmark(const base::DictValue& dict) {
  SharedBookmark bookmark;
  if (const std::string* title = dict.FindString("title")) {
    bookmark.title = *title;
  }
  if (const std::string* url = dict.FindString("url")) {
    bookmark.url = *url;
  }
  if (const std::string* icon = dict.FindString("icon")) {
    bookmark.icon = *icon;
  }
  if (bookmark.title.empty()) {
    bookmark.title = bookmark.url;
  }
  if (ShouldRefreshIcon(bookmark.icon)) {
    bookmark.icon = FaviconForUrl(bookmark.url);
  }
  return bookmark;
}

base::DictValue BookmarkToDict(const SharedBookmark& bookmark) {
  base::DictValue dict;
  dict.Set("title", bookmark.title);
  dict.Set("url", bookmark.url);
  dict.Set("icon", bookmark.icon);
  return dict;
}

std::vector<SharedBookmark> Load() {
  std::vector<SharedBookmark> out;
  const base::FilePath path = StorePath();
  std::string contents;
  if (path.empty() || !base::ReadFileToString(path, &contents)) {
    return out;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root) {
    return out;
  }
  const base::ListValue* list = root->FindList("bookmarks");
  if (!list) {
    return out;
  }
  for (const base::Value& value : *list) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    SharedBookmark bookmark = DictToBookmark(*dict);
    if (!bookmark.url.empty()) {
      out.push_back(std::move(bookmark));
    }
  }
  return out;
}

void Save(const std::vector<SharedBookmark>& bookmarks) {
  const base::FilePath path = StorePath();
  if (path.empty() || !base::CreateDirectory(path.DirName())) {
    return;
  }
  base::DictValue root;
  base::ListValue list;
  for (const SharedBookmark& bookmark : bookmarks) {
    if (!bookmark.url.empty()) {
      list.Append(BookmarkToDict(bookmark));
    }
  }
  root.Set("bookmarks", std::move(list));
  if (std::optional<std::string> json = base::WriteJson(root)) {
    base::WriteFile(path, *json);
  }
}

std::string ExportJson(const std::vector<SharedBookmark>& bookmarks) {
  base::DictValue root;
  base::ListValue list;
  for (const SharedBookmark& bookmark : bookmarks) {
    if (!bookmark.url.empty()) {
      list.Append(BookmarkToDict(bookmark));
    }
  }
  root.Set("bookmarks", std::move(list));
  return base::WriteJson(root).value_or("{}");
}

}  // namespace

std::vector<SharedBookmark> GetSharedBookmarks() {
  return Load();
}

std::string ExportSharedBookmarksJson() {
  return ExportJson(Load());
}

void ReplaceSharedBookmarksFromJson(const std::string& json) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* list = root->FindList("bookmarks");
  if (!list) {
    return;
  }
  std::vector<SharedBookmark> bookmarks;
  for (const base::Value& value : *list) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    SharedBookmark bookmark = DictToBookmark(*dict);
    if (!bookmark.url.empty()) {
      bookmarks.push_back(std::move(bookmark));
    }
  }
  Save(bookmarks);
}

void AddOrUpdateSharedBookmark(const std::string& title,
                               const std::string& url) {
  const std::string normalized = NormalizeUrl(url);
  if (normalized.empty()) {
    return;
  }
  std::vector<SharedBookmark> bookmarks = Load();
  SharedBookmark bookmark;
  bookmark.title = title.empty() ? normalized : title;
  bookmark.url = normalized;
  bookmark.icon = FaviconForUrl(normalized);
  for (SharedBookmark& existing : bookmarks) {
    if (existing.url == normalized) {
      existing = bookmark;
      Save(bookmarks);
      return;
    }
  }
  bookmarks.push_back(std::move(bookmark));
  Save(bookmarks);
}

void RemoveSharedBookmark(const std::string& url) {
  const std::string normalized = NormalizeUrl(url);
  std::vector<SharedBookmark> bookmarks = Load();
  std::erase_if(bookmarks, [&normalized](const SharedBookmark& bookmark) {
    return bookmark.url == normalized;
  });
  Save(bookmarks);
}

}  // namespace monti

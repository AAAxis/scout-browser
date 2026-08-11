// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_extension_store.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/values.h"
#include "chrome/common/chrome_paths.h"

namespace monti {
namespace {

constexpr char kLaunchSwitchesPath[] = "monti://launch-switches";

base::FilePath StorePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.AppendASCII("Monti").AppendASCII("extensions.json");
}

struct SharedExtensionState {
  std::vector<SharedExtension> extensions;
};

SharedExtensionState LoadState() {
  SharedExtensionState state;
  const base::FilePath path = StorePath();
  std::string contents;
  if (path.empty() || !base::ReadFileToString(path, &contents)) {
    return state;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root) {
    return state;
  }
  const base::ListValue* list = root->FindList("extensions");
  if (!list) {
    return state;
  }
  for (const base::Value& value : *list) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string* stored_path = dict->FindString("path");
    if (!stored_path || stored_path->empty()) {
      continue;
    }
    if (*stored_path == kLaunchSwitchesPath) {
      continue;
    }
    SharedExtension extension;
    extension.path = base::FilePath::FromUTF8Unsafe(*stored_path);
    if (const std::string* name = dict->FindString("name")) {
      extension.name = *name;
    }
    if (extension.name.empty()) {
      extension.name = extension.path.BaseName().AsUTF8Unsafe();
    }
    state.extensions.push_back(std::move(extension));
  }
  return state;
}

void SaveState(const SharedExtensionState& state) {
  const base::FilePath path = StorePath();
  if (path.empty() || !base::CreateDirectory(path.DirName())) {
    return;
  }
  base::DictValue root;
  base::ListValue list;
  for (const SharedExtension& extension : state.extensions) {
    if (extension.path.empty()) {
      continue;
    }
    base::DictValue dict;
    dict.Set("name", extension.name);
    dict.Set("path", extension.path.AsUTF8Unsafe());
    list.Append(std::move(dict));
  }
  root.Set("extensions", std::move(list));
  std::optional<std::string> json = base::WriteJson(root);
  if (json) {
    base::WriteFile(path, *json);
  }
}

std::string ExportJson(const SharedExtensionState& state) {
  base::DictValue root;
  base::ListValue list;
  for (const SharedExtension& extension : state.extensions) {
    if (extension.path.empty()) {
      continue;
    }
    base::DictValue dict;
    dict.Set("name", extension.name);
    dict.Set("path", extension.path.AsUTF8Unsafe());
    list.Append(std::move(dict));
  }
  root.Set("extensions", std::move(list));
  return base::WriteJson(root).value_or("{}");
}

}  // namespace

std::vector<SharedExtension> GetSharedExtensions() {
  return LoadState().extensions;
}

std::vector<base::FilePath> GetSharedExtensionPaths() {
  std::vector<base::FilePath> paths;
  for (const SharedExtension& extension : LoadState().extensions) {
    if (!extension.path.empty()) {
      paths.push_back(extension.path);
    }
  }
  return paths;
}

std::string GetSharedLaunchSwitches() {
  return std::string();
}

std::string ExportSharedExtensionsJson() {
  return ExportJson(LoadState());
}

void ReplaceSharedExtensionsFromJson(const std::string& json) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* list = root->FindList("extensions");
  if (!list) {
    return;
  }

  SharedExtensionState state;
  for (const base::Value& value : *list) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string* stored_path = dict->FindString("path");
    if (!stored_path || stored_path->empty()) {
      continue;
    }
    if (*stored_path == kLaunchSwitchesPath) {
      continue;
    }
    SharedExtension extension;
    extension.path = base::FilePath::FromUTF8Unsafe(*stored_path);
    if (const std::string* name = dict->FindString("name")) {
      extension.name = *name;
    }
    if (extension.name.empty()) {
      extension.name = extension.path.BaseName().AsUTF8Unsafe();
    }
    state.extensions.push_back(std::move(extension));
  }
  SaveState(state);
}

void AddSharedExtensionPath(const base::FilePath& path) {
  if (path.empty()) {
    return;
  }
  SharedExtensionState state = LoadState();
  for (const SharedExtension& extension : state.extensions) {
    if (extension.path == path) {
      return;
    }
  }
  SharedExtension extension;
  extension.path = path;
  extension.name = path.BaseName().AsUTF8Unsafe();
  state.extensions.push_back(std::move(extension));
  SaveState(state);
}

void RemoveSharedExtensionPath(const base::FilePath& path) {
  SharedExtensionState state = LoadState();
  std::erase_if(state.extensions, [&path](const SharedExtension& extension) {
    return extension.path == path;
  });
  SaveState(state);
}

void SetSharedLaunchSwitches(const std::string&) {
}

}  // namespace monti

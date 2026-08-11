// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/folder_store.h"

#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/common/chrome_paths.h"

namespace monti {

namespace {

base::FilePath GetFoldersFilePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.AppendASCII("Monti").AppendASCII("folders.json");
}

base::DictValue FolderToDict(const Folder& folder) {
  base::DictValue dict;
  dict.Set("id", folder.id);
  dict.Set("parent_id", folder.parent_id);
  dict.Set("name", folder.name);
  return dict;
}

Folder DictToFolder(const base::DictValue& dict) {
  Folder folder;
  if (const std::string* v = dict.FindString("id")) {
    folder.id = *v;
  }
  if (const std::string* v = dict.FindString("parent_id")) {
    folder.parent_id = *v;
  }
  if (const std::string* v = dict.FindString("name")) {
    folder.name = *v;
  }
  return folder;
}

// Runs on the I/O sequence (blocking allowed there). Reads and parses the
// folders file, returning an empty vector if it is missing or malformed.
std::vector<Folder> LoadFromDisk(base::FilePath path) {
  std::vector<Folder> folders;
  if (path.empty()) {
    return folders;
  }

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return folders;  // No file yet: start empty.
  }

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root) {
    return folders;
  }

  const base::ListValue* entries = root->FindList("folders");
  if (!entries) {
    return folders;
  }

  for (const base::Value& entry : *entries) {
    if (entry.is_dict()) {
      folders.push_back(DictToFolder(entry.GetDict()));
    }
  }
  return folders;
}

// Runs on the I/O sequence (blocking allowed there). Writes the serialized
// store, creating <user-data>/Monti if needed.
void SaveToDisk(base::FilePath path, std::string json) {
  if (path.empty() || !base::CreateDirectory(path.DirName())) {
    return;
  }
  base::WriteFile(path, json);
}

}  // namespace

// static
FolderStore* FolderStore::GetInstance() {
  static base::NoDestructor<FolderStore> instance;
  return instance.get();
}

FolderStore::FolderStore()
    : io_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}
FolderStore::~FolderStore() = default;

const std::vector<Folder>& FolderStore::list() {
  EnsureLoaded();
  return folders_;
}

const Folder* FolderStore::GetById(const std::string& id) {
  EnsureLoaded();
  for (const Folder& folder : folders_) {
    if (folder.id == id) {
      return &folder;
    }
  }
  return nullptr;
}

void FolderStore::Add(const Folder& folder) {
  EnsureLoaded();
  for (Folder& existing : folders_) {
    if (existing.id == folder.id) {
      existing = folder;
      ScheduleSave();
      return;
    }
  }
  folders_.push_back(folder);
  ScheduleSave();
}

void FolderStore::Update(const Folder& folder) {
  EnsureLoaded();
  for (Folder& existing : folders_) {
    if (existing.id == folder.id) {
      existing = folder;
      ScheduleSave();
      return;
    }
  }
}

void FolderStore::Remove(const std::string& id) {
  EnsureLoaded();
  for (auto it = folders_.begin(); it != folders_.end(); ++it) {
    if (it->id == id) {
      folders_.erase(it);
      ScheduleSave();
      return;
    }
  }
}

std::string FolderStore::ExportJson() {
  EnsureLoaded();
  base::ListValue list;
  for (const Folder& folder : folders_) {
    list.Append(FolderToDict(folder));
  }
  base::DictValue root;
  root.Set("folders", std::move(list));
  return base::WriteJson(root).value_or("{}");
}

void FolderStore::ReplaceFromJson(const std::string& json) {
  EnsureLoaded();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* entries = root->FindList("folders");
  if (!entries) {
    return;
  }

  std::vector<Folder> folders;
  for (const base::Value& value : *entries) {
    if (!value.is_dict()) {
      continue;
    }
    Folder folder = DictToFolder(value.GetDict());
    if (!folder.id.empty()) {
      folders.push_back(std::move(folder));
    }
  }
  folders_ = std::move(folders);
  ScheduleSave();
}

void FolderStore::EnsureLoaded() {
  if (loaded_) {
    return;
  }
  loaded_ = true;

  // Read the file off the UI thread, then merge the result back in. The store
  // is a process-wide NoDestructor singleton, so base::Unretained(this) is safe
  // for the reply.
  io_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&LoadFromDisk, GetFoldersFilePath()),
      base::BindOnce(&FolderStore::OnLoaded, base::Unretained(this)));
}

void FolderStore::OnLoaded(std::vector<Folder> loaded) {
  if (folders_.empty()) {
    // Common case: nothing was added before the load finished. Adopt the disk
    // contents verbatim; no need to write them straight back out.
    folders_ = std::move(loaded);
    return;
  }

  // Something was added before the async load completed. Keep the in-memory
  // entries authoritative and fold in any disk-only ids, then persist the union.
  bool changed = false;
  for (Folder& disk_folder : loaded) {
    bool present = false;
    for (const Folder& existing : folders_) {
      if (existing.id == disk_folder.id) {
        present = true;
        break;
      }
    }
    if (!present) {
      folders_.push_back(std::move(disk_folder));
      changed = true;
    }
  }
  if (changed) {
    ScheduleSave();
  }
}

void FolderStore::ScheduleSave() {
  base::FilePath path = GetFoldersFilePath();
  if (path.empty()) {
    return;
  }

  std::string json = ExportJson();
  if (json.empty()) {
    return;
  }
  io_runner_->PostTask(
      FROM_HERE, base::BindOnce(&SaveToDisk, std::move(path), std::move(json)));
}

}  // namespace monti

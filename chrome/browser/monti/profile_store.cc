// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/profile_store.h"

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

base::FilePath GetProfilesFilePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.AppendASCII("Monti").AppendASCII("profiles.json");
}

base::DictValue EntryToDict(const MontiProfileEntry& entry) {
  base::DictValue dict;
  dict.Set("id", entry.id);
  dict.Set("name", entry.name);
  dict.Set("profile_dir", entry.profile_dir);
  dict.Set("assigned_proxy_id", entry.assigned_proxy_id);
  dict.Set("created_at", entry.created_at);
  dict.Set("profile_status", entry.profile_status);

  base::ListValue tags;
  for (const std::string& tag : entry.tags) {
    tags.Append(tag);
  }
  dict.Set("tags", std::move(tags));
  dict.Set("notes", entry.notes);
  dict.Set("color", entry.color);
  dict.Set("folder_id", entry.folder_id);
  dict.Set("ua_preset", entry.ua_preset);
  // Store the fingerprint (held as a JSON string) as a nested object so the
  // registry file stays human-inspectable. Skipped when unset.
  if (!entry.fingerprint.empty()) {
    if (std::optional<base::DictValue> fp = base::JSONReader::ReadDict(
            entry.fingerprint, base::JSON_PARSE_RFC)) {
      dict.Set("fingerprint", std::move(*fp));
    }
  }
  return dict;
}

MontiProfileEntry DictToEntry(const base::DictValue& dict) {
  MontiProfileEntry entry;
  if (const std::string* v = dict.FindString("id")) {
    entry.id = *v;
  }
  if (const std::string* v = dict.FindString("name")) {
    entry.name = *v;
  }
  if (const std::string* v = dict.FindString("profile_dir")) {
    entry.profile_dir = *v;
  }
  if (const std::string* v = dict.FindString("assigned_proxy_id")) {
    entry.assigned_proxy_id = *v;
  }
  if (const std::string* v = dict.FindString("created_at")) {
    entry.created_at = *v;
  }
  if (const std::string* v = dict.FindString("profile_status")) {
    entry.profile_status = *v;
  }
  if (const base::ListValue* tags = dict.FindList("tags")) {
    for (const base::Value& tag : *tags) {
      if (tag.is_string()) {
        entry.tags.push_back(tag.GetString());
      }
    }
  }
  if (const std::string* v = dict.FindString("notes")) {
    entry.notes = *v;
  }
  if (const std::string* v = dict.FindString("color")) {
    entry.color = *v;
  }
  if (const std::string* v = dict.FindString("folder_id")) {
    entry.folder_id = *v;
  }
  if (const std::string* v = dict.FindString("ua_preset")) {
    entry.ua_preset = *v;
  }
  if (const base::DictValue* fp = dict.FindDict("fingerprint")) {
    if (std::optional<std::string> json = base::WriteJson(*fp)) {
      entry.fingerprint = std::move(*json);
    }
  }
  return entry;
}

// Runs on the I/O sequence (blocking allowed there). Reads and parses the
// profiles file, returning an empty vector if it is missing or malformed.
std::vector<MontiProfileEntry> LoadFromDisk(base::FilePath path) {
  std::vector<MontiProfileEntry> profiles;
  if (path.empty()) {
    return profiles;
  }

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return profiles;  // No file yet: start empty.
  }

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root) {
    return profiles;
  }

  const base::ListValue* entries = root->FindList("profiles");
  if (!entries) {
    return profiles;
  }

  for (const base::Value& entry : *entries) {
    if (entry.is_dict()) {
      profiles.push_back(DictToEntry(entry.GetDict()));
    }
  }
  return profiles;
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
ProfileStore* ProfileStore::GetInstance() {
  static base::NoDestructor<ProfileStore> instance;
  return instance.get();
}

ProfileStore::ProfileStore()
    : io_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}
ProfileStore::~ProfileStore() = default;

const std::vector<MontiProfileEntry>& ProfileStore::list() {
  EnsureLoaded();
  return profiles_;
}

const MontiProfileEntry* ProfileStore::GetById(const std::string& id) {
  EnsureLoaded();
  for (const MontiProfileEntry& entry : profiles_) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

void ProfileStore::Add(const MontiProfileEntry& entry) {
  EnsureLoaded();
  for (MontiProfileEntry& existing : profiles_) {
    if (existing.id == entry.id) {
      existing = entry;
      ScheduleSave();
      return;
    }
  }
  profiles_.push_back(entry);
  ScheduleSave();
}

void ProfileStore::Update(const MontiProfileEntry& entry) {
  EnsureLoaded();
  for (MontiProfileEntry& existing : profiles_) {
    if (existing.id == entry.id) {
      existing = entry;
      ScheduleSave();
      return;
    }
  }
}

void ProfileStore::Remove(const std::string& id) {
  EnsureLoaded();
  for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
    if (it->id == id) {
      profiles_.erase(it);
      ScheduleSave();
      return;
    }
  }
}

std::string ProfileStore::ExportJson() {
  EnsureLoaded();
  base::ListValue list;
  for (const MontiProfileEntry& entry : profiles_) {
    list.Append(EntryToDict(entry));
  }
  base::DictValue root;
  root.Set("profiles", std::move(list));
  return base::WriteJson(root).value_or("{}");
}

void ProfileStore::MergeFromJson(const std::string& json) {
  EnsureLoaded();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* entries = root->FindList("profiles");
  if (!entries) {
    return;
  }

  bool changed = false;
  for (const base::Value& value : *entries) {
    if (!value.is_dict()) {
      continue;
    }
    MontiProfileEntry incoming = DictToEntry(value.GetDict());
    if (incoming.id.empty()) {
      continue;
    }
    bool present = false;
    for (const MontiProfileEntry& existing : profiles_) {
      if (existing.id == incoming.id) {
        present = true;
        break;
      }
    }
    if (!present) {
      profiles_.push_back(std::move(incoming));
      changed = true;
    }
  }
  if (changed) {
    ScheduleSave();
  }
}

void ProfileStore::ReplaceFromJson(const std::string& json) {
  EnsureLoaded();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* entries = root->FindList("profiles");
  if (!entries) {
    return;
  }

  std::vector<MontiProfileEntry> profiles;
  for (const base::Value& value : *entries) {
    if (!value.is_dict()) {
      continue;
    }
    MontiProfileEntry incoming = DictToEntry(value.GetDict());
    if (!incoming.id.empty()) {
      profiles.push_back(std::move(incoming));
    }
  }
  profiles_ = std::move(profiles);
  ScheduleSave();
}

void ProfileStore::EnsureLoaded() {
  if (loaded_) {
    return;
  }
  loaded_ = true;

  // Read the file off the UI thread, then merge the result back in. The store
  // is a process-wide NoDestructor singleton, so base::Unretained(this) is safe
  // for the reply.
  io_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&LoadFromDisk, GetProfilesFilePath()),
      base::BindOnce(&ProfileStore::OnLoaded, base::Unretained(this)));
}

void ProfileStore::OnLoaded(std::vector<MontiProfileEntry> loaded) {
  if (profiles_.empty()) {
    // Common case: nothing was added before the load finished. Adopt the disk
    // contents verbatim; no need to write them straight back out.
    profiles_ = std::move(loaded);
    return;
  }

  // Something was added before the async load completed. Keep the in-memory
  // entries authoritative and fold in any disk-only ids, then persist the union
  // so disk reflects both.
  bool changed = false;
  for (MontiProfileEntry& disk_entry : loaded) {
    bool present = false;
    for (const MontiProfileEntry& existing : profiles_) {
      if (existing.id == disk_entry.id) {
        present = true;
        break;
      }
    }
    if (!present) {
      profiles_.push_back(std::move(disk_entry));
      changed = true;
    }
  }
  if (changed) {
    ScheduleSave();
  }
}

void ProfileStore::ScheduleSave() {
  base::FilePath path = GetProfilesFilePath();
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

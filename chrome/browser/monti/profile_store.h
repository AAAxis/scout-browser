// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_PROFILE_STORE_H_
#define CHROME_BROWSER_MONTI_PROFILE_STORE_H_

#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/monti/monti_profile_entry.h"

namespace monti {

// Process-wide registry of the user's Monti profiles, persisted as JSON in
// <user-data>/Monti/profiles.json. The in-memory vector is the source of truth;
// it is populated by an asynchronous load on first access and written back by
// asynchronous saves. All disk I/O runs on a MayBlock() ThreadPool sequence so
// the UI thread (which forbids blocking calls) never touches the filesystem.
// Intended to be used from the UI thread for the MVP. Mirrors ProxyStore.
class ProfileStore {
 public:
  static ProfileStore* GetInstance();

  ProfileStore(const ProfileStore&) = delete;
  ProfileStore& operator=(const ProfileStore&) = delete;

  // Returns all profiles (in insertion order).
  const std::vector<MontiProfileEntry>& list();

  // Returns the profile with the given id, or nullptr if none.
  const MontiProfileEntry* GetById(const std::string& id);

  // Inserts or replaces (matched by id) a single profile, then persists.
  void Add(const MontiProfileEntry& entry);

  // Updates an existing profile matched by id (no-op if absent), then persists.
  void Update(const MontiProfileEntry& entry);

  // Removes the profile with the given id (no-op if absent), then persists.
  void Remove(const std::string& id);

  // Serializes/deserializes the registry shape used by profiles.json. Cloud
  // sync stores this exact payload so local disk and cloud stay compatible.
  std::string ExportJson();
  void MergeFromJson(const std::string& json);
  void ReplaceFromJson(const std::string& json);

 private:
  friend class base::NoDestructor<ProfileStore>;

  ProfileStore();
  ~ProfileStore();

  // Kicks off the one-time asynchronous load from disk (no-op afterwards).
  void EnsureLoaded();
  // Reply for the async load: merges any entries from disk that aren't already
  // present in memory (so a write that happened before the load completed is
  // not lost), then persists the union if it changed.
  void OnLoaded(std::vector<MontiProfileEntry> loaded);
  // Serializes the current set on the calling sequence and posts the write to
  // the I/O sequence (fire-and-forget).
  void ScheduleSave();

  bool loaded_ = false;
  std::vector<MontiProfileEntry> profiles_;
  scoped_refptr<base::SequencedTaskRunner> io_runner_;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_PROFILE_STORE_H_

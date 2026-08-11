// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_FOLDER_STORE_H_
#define CHROME_BROWSER_MONTI_FOLDER_STORE_H_

#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/monti/monti_folder.h"

namespace monti {

// Process-wide registry of the folders the user creates to organize profiles,
// persisted as JSON in <user-data>/Monti/folders.json. The in-memory vector is
// the source of truth; it is populated by an asynchronous load on first access
// and written back by asynchronous saves. All disk I/O runs on a MayBlock()
// ThreadPool sequence so the UI thread (which forbids blocking calls) never
// touches the filesystem. Intended for the UI thread for the MVP. Mirrors
// ProxyStore.
class FolderStore {
 public:
  static FolderStore* GetInstance();

  FolderStore(const FolderStore&) = delete;
  FolderStore& operator=(const FolderStore&) = delete;

  // Returns all folders (in insertion order).
  const std::vector<Folder>& list();

  // Returns the folder with the given id, or nullptr if none.
  const Folder* GetById(const std::string& id);

  // Inserts or replaces (matched by id) a single folder, then persists.
  void Add(const Folder& folder);

  // Updates an existing folder matched by id (no-op if absent), then persists.
  void Update(const Folder& folder);

  // Removes the folder with the given id (no-op if absent), then persists.
  void Remove(const std::string& id);

  std::string ExportJson();
  void ReplaceFromJson(const std::string& json);

 private:
  friend class base::NoDestructor<FolderStore>;

  FolderStore();
  ~FolderStore();

  // Kicks off the one-time asynchronous load from disk (no-op afterwards).
  void EnsureLoaded();
  // Reply for the async load: merges any folders from disk that aren't already
  // present in memory, then persists the union if it changed.
  void OnLoaded(std::vector<Folder> loaded);
  // Serializes the current set on the calling sequence and posts the write to
  // the I/O sequence (fire-and-forget).
  void ScheduleSave();

  bool loaded_ = false;
  std::vector<Folder> folders_;
  scoped_refptr<base::SequencedTaskRunner> io_runner_;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_FOLDER_STORE_H_

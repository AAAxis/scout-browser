// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_PROXY_STORE_H_
#define CHROME_BROWSER_MONTI_PROXY_STORE_H_

#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/monti/monti_proxy.h"

namespace monti {

// Process-wide store of the user's proxies, persisted as JSON in
// <user-data>/Monti/proxies.json. The in-memory vector is the source of truth;
// it is populated by an asynchronous load on first access and written back by
// asynchronous saves. All disk I/O runs on a MayBlock() ThreadPool sequence so
// the UI thread (which forbids blocking calls) never touches the filesystem.
// Intended to be used from the UI thread for the MVP.
class ProxyStore {
 public:
  static ProxyStore* GetInstance();

  ProxyStore(const ProxyStore&) = delete;
  ProxyStore& operator=(const ProxyStore&) = delete;

  // Returns all proxies (in insertion order).
  const std::vector<MontiProxy>& list();

  // Returns the proxy with the given id, or nullptr if none.
  const MontiProxy* GetById(const std::string& id);

  // Inserts or replaces (matched by id) a single proxy, then persists.
  void Add(const MontiProxy& proxy);

  // Updates an existing proxy matched by id (no-op if absent), then persists.
  void Update(const MontiProxy& proxy);

  // Removes the proxy with the given id (no-op if absent), then persists.
  void Remove(const std::string& id);

  // Replaces the entire set (used by CSV import), then persists.
  void ReplaceAll(std::vector<MontiProxy> proxies);

  std::string ExportJson();
  void ReplaceFromJson(const std::string& json);

 private:
  friend class base::NoDestructor<ProxyStore>;

  ProxyStore();
  ~ProxyStore();

  // Kicks off the one-time asynchronous load from disk (no-op afterwards).
  void EnsureLoaded();
  // Reply for the async load: merges any entries from disk that aren't already
  // present in memory (so a write that happened before the load completed is
  // not lost), then persists the union if it changed.
  void OnLoaded(std::vector<MontiProxy> loaded);
  // Serializes the current set on the calling sequence and posts the write to
  // the I/O sequence (fire-and-forget).
  void ScheduleSave();

  bool loaded_ = false;
  std::vector<MontiProxy> proxies_;
  scoped_refptr<base::SequencedTaskRunner> io_runner_;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_PROXY_STORE_H_

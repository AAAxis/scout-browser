// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/proxy_store.h"

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

base::FilePath GetProxiesFilePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.AppendASCII("Monti").AppendASCII("proxies.json");
}

base::DictValue ProxyToDict(const MontiProxy& proxy) {
  base::DictValue dict;
  dict.Set("id", proxy.id);
  dict.Set("name", proxy.name);
  dict.Set("host", proxy.host);
  dict.Set("http_port", proxy.http_port);
  dict.Set("socks_port", proxy.socks_port);
  dict.Set("username", proxy.username);
  dict.Set("password", proxy.password);
  dict.Set("country", proxy.country);
  dict.Set("status", proxy.status);
  dict.Set("assigned_profile", proxy.assigned_profile);
  dict.Set("transport", proxy.transport);
  return dict;
}

MontiProxy DictToProxy(const base::DictValue& dict) {
  MontiProxy proxy;
  if (const std::string* v = dict.FindString("id")) {
    proxy.id = *v;
  }
  if (const std::string* v = dict.FindString("host")) {
    proxy.host = *v;
  }
  proxy.http_port = dict.FindInt("http_port").value_or(0);
  proxy.socks_port = dict.FindInt("socks_port").value_or(0);
  if (const std::string* v = dict.FindString("username")) {
    proxy.username = *v;
  }
  if (const std::string* v = dict.FindString("password")) {
    proxy.password = *v;
  }
  if (const std::string* v = dict.FindString("country")) {
    proxy.country = *v;
  }
  if (const std::string* v = dict.FindString("status")) {
    proxy.status = *v;
  }
  if (const std::string* v = dict.FindString("assigned_profile")) {
    proxy.assigned_profile = *v;
  }
  if (const std::string* v = dict.FindString("transport")) {
    proxy.transport = *v;
  }
  // `name` is additive: old proxies.json files have no name, so derive a label
  // from the country and host when it is absent.
  if (const std::string* v = dict.FindString("name"); v && !v->empty()) {
    proxy.name = *v;
  } else {
    proxy.name = proxy.country.empty()
                     ? proxy.host
                     : proxy.country + " · " + proxy.host;
  }
  return proxy;
}

// Runs on the I/O sequence (blocking allowed there). Reads and parses the
// proxies file, returning an empty vector if it is missing or malformed.
std::vector<MontiProxy> LoadFromDisk(base::FilePath path) {
  std::vector<MontiProxy> proxies;
  if (path.empty()) {
    return proxies;
  }

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return proxies;  // No file yet: start empty.
  }

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root) {
    return proxies;
  }

  const base::ListValue* entries = root->FindList("proxies");
  if (!entries) {
    return proxies;
  }

  for (const base::Value& entry : *entries) {
    if (entry.is_dict()) {
      proxies.push_back(DictToProxy(entry.GetDict()));
    }
  }
  return proxies;
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
ProxyStore* ProxyStore::GetInstance() {
  static base::NoDestructor<ProxyStore> instance;
  return instance.get();
}

ProxyStore::ProxyStore()
    : io_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

ProxyStore::~ProxyStore() = default;

const std::vector<MontiProxy>& ProxyStore::list() {
  EnsureLoaded();
  return proxies_;
}

const MontiProxy* ProxyStore::GetById(const std::string& id) {
  EnsureLoaded();
  for (const MontiProxy& proxy : proxies_) {
    if (proxy.id == id) {
      return &proxy;
    }
  }
  return nullptr;
}

void ProxyStore::Add(const MontiProxy& proxy) {
  EnsureLoaded();
  for (MontiProxy& existing : proxies_) {
    if (existing.id == proxy.id) {
      existing = proxy;
      ScheduleSave();
      return;
    }
  }
  proxies_.push_back(proxy);
  ScheduleSave();
}

void ProxyStore::Update(const MontiProxy& proxy) {
  EnsureLoaded();
  for (MontiProxy& existing : proxies_) {
    if (existing.id == proxy.id) {
      existing = proxy;
      ScheduleSave();
      return;
    }
  }
}

void ProxyStore::Remove(const std::string& id) {
  EnsureLoaded();
  for (auto it = proxies_.begin(); it != proxies_.end(); ++it) {
    if (it->id == id) {
      proxies_.erase(it);
      ScheduleSave();
      return;
    }
  }
}

void ProxyStore::ReplaceAll(std::vector<MontiProxy> proxies) {
  EnsureLoaded();
  proxies_ = std::move(proxies);
  ScheduleSave();
}

std::string ProxyStore::ExportJson() {
  EnsureLoaded();
  base::ListValue list;
  for (const MontiProxy& proxy : proxies_) {
    list.Append(ProxyToDict(proxy));
  }
  base::DictValue root;
  root.Set("proxies", std::move(list));
  return base::WriteJson(root).value_or("{}");
}

void ProxyStore::ReplaceFromJson(const std::string& json) {
  EnsureLoaded();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* entries = root->FindList("proxies");
  if (!entries) {
    return;
  }

  std::vector<MontiProxy> proxies;
  for (const base::Value& value : *entries) {
    if (!value.is_dict()) {
      continue;
    }
    MontiProxy proxy = DictToProxy(value.GetDict());
    if (!proxy.id.empty()) {
      proxies.push_back(std::move(proxy));
    }
  }
  proxies_ = std::move(proxies);
  ScheduleSave();
}

void ProxyStore::EnsureLoaded() {
  if (loaded_) {
    return;
  }
  loaded_ = true;

  // Read the file off the UI thread, then merge the result back in. The store
  // is a process-wide NoDestructor singleton, so base::Unretained(this) is safe
  // for the reply.
  io_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&LoadFromDisk, GetProxiesFilePath()),
      base::BindOnce(&ProxyStore::OnLoaded, base::Unretained(this)));
}

void ProxyStore::OnLoaded(std::vector<MontiProxy> loaded) {
  // Purge legacy side-panel/Doppler entries: those were auto-injected by the
  // VPN connect path and never belong in the user-managed proxy list. Dropping
  // them here permanently cleans them out of proxies.json on the next save.
  const size_t before = loaded.size();
  std::erase_if(loaded, [](const MontiProxy& p) {
    return p.id.rfind("doppler:", 0) == 0;
  });
  const bool dropped_doppler = loaded.size() != before;

  if (proxies_.empty()) {
    // Common case: nothing was added before the load finished. Adopt the disk
    // contents verbatim. Re-persist only if we stripped a stale Doppler entry.
    proxies_ = std::move(loaded);
    if (dropped_doppler) {
      ScheduleSave();
    }
    return;
  }

  // Something was added (e.g. a Connect) before the async load completed. Keep
  // the in-memory entries authoritative and fold in any disk-only ids, then
  // persist the union so disk reflects both.
  bool changed = false;
  for (MontiProxy& disk_proxy : loaded) {
    bool present = false;
    for (const MontiProxy& existing : proxies_) {
      if (existing.id == disk_proxy.id) {
        present = true;
        break;
      }
    }
    if (!present) {
      proxies_.push_back(std::move(disk_proxy));
      changed = true;
    }
  }
  if (changed) {
    ScheduleSave();
  }
}

void ProxyStore::ScheduleSave() {
  base::FilePath path = GetProxiesFilePath();
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

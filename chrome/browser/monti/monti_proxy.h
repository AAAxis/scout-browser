// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROXY_H_
#define CHROME_BROWSER_MONTI_MONTI_PROXY_H_

#include <string>

namespace monti {

// A single proxy entry. Plain data; owned and persisted by ProxyStore.
struct MontiProxy {
  std::string id;                    // from CSV "id", or generated
  std::string name;                  // user label; derived (country · host) if absent
  std::string host;                  // CSV "ip"
  int http_port = 0;                 // CSV "port_http"
  int socks_port = 0;                // CSV "port_socks5"
  std::string username;              // CSV "username"
  std::string password;              // CSV "password"
  std::string country;               // CSV "country"
  std::string status = "unchecked";  // "unchecked" | "ok" | "fail" | "checking"
  std::string assigned_profile;      // profile id or empty
  std::string transport = "http";    // "http" | "socks5"
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROXY_H_

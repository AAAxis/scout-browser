// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/proxy_applicator.h"

#include <cstdint>
#include <string>
#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/auth.h"
#include "net/base/network_anonymization_key.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/scheme_host_port.h"

namespace monti {

namespace {

// Do not bypass localhost. Browser leak checks commonly probe ports on
// 127.0.0.1/localhost to detect local apps and services such as remote desktop
// tools or automation APIs. Keep those destinations on the proxy path instead
// of connecting directly to the user's device.
constexpr char kNoProxyBypassRules[] = "";

}  // namespace

void ApplyProxyToProfile(Profile* profile, const MontiProxy& proxy) {
  // 1. Point the profile at the proxy's HTTP port for both http and https
  //    traffic. ProxyConfigMonitor observes this pref and pushes the config to
  //    the profile's NetworkContext automatically.
  const std::string server =
      base::StringPrintf("http=%s:%d;https=%s:%d", proxy.host.c_str(),
                         proxy.http_port, proxy.host.c_str(), proxy.http_port);
  profile->GetPrefs()->SetDict(
      proxy_config::prefs::kProxy,
      ProxyConfigDictionary::CreateFixedServers(server, kNoProxyBypassRules));

  // 2. Preemptively seed the proxy credentials into the network context's
  //    HttpAuthCache so the browser never shows an interactive 407 dialog.
  //    NOTE: Basic proxy entries are keyed by realm, so this preemptive seed
  //    only matches if the proxy's 407 advertises an empty realm. Providers
  //    that send a named realm will still 407 on the first request (surfaced
  //    now as "Proxy authentication failed (HTTP 407)" in the probe). The
  //    reliable, realm-independent path for authenticated proxies is the SOCKS5
  //    bridge (see SocksBridge), which the CSV importer auto-selects whenever a
  //    SOCKS port is present.
  net::AuthChallengeInfo challenge;
  challenge.is_proxy = true;
  challenge.challenger = url::SchemeHostPort(
      "http", proxy.host, static_cast<uint16_t>(proxy.http_port));
  challenge.scheme = "basic";
  challenge.realm = std::string();

  net::AuthCredentials credentials(base::UTF8ToUTF16(proxy.username),
                                   base::UTF8ToUTF16(proxy.password));

  profile->GetDefaultStoragePartition()
      ->GetNetworkContext()
      ->AddAuthCacheEntry(challenge, net::NetworkAnonymizationKey(),
                          credentials, base::DoNothing());
}

void ApplySocksProxyToProfile(Profile* profile, int local_port) {
  // Point the profile at the local SOCKS5 bridge. The server must be expressed
  // as a proxy URI ("socks5://host:port"): Chromium's proxy-rules parser only
  // recognizes the scheme keys http=/https=/ftp=/socks= before an '=', so a
  // "socks5=..." entry would be silently dropped and traffic would go direct.
  // Written without an '=', this becomes a single proxy applied to all traffic,
  // with the socks5:// scheme selecting SOCKS5.
  const std::string server =
      base::StringPrintf("socks5://127.0.0.1:%d", local_port);
  profile->GetPrefs()->SetDict(
      proxy_config::prefs::kProxy,
      ProxyConfigDictionary::CreateFixedServers(server, kNoProxyBypassRules));
}

void ClearProxyFromProfile(Profile* profile) {
  profile->GetPrefs()->SetDict(proxy_config::prefs::kProxy,
                               ProxyConfigDictionary::CreateDirect());
}

}  // namespace monti

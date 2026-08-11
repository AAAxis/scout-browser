// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_SOCKS_BRIDGE_H_
#define CHROME_BROWSER_MONTI_SOCKS_BRIDGE_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/threading/thread.h"
#include "chrome/browser/monti/monti_proxy.h"

namespace monti {

// Browser-process-lifetime forwarder that lets Chromium use SOCKS5 proxies
// which require username/password (RFC 1929) authentication. Chromium's own
// SOCKS5 client only speaks SOCKS5 "no auth", so for each authenticated
// upstream proxy this manager opens a listener on 127.0.0.1:<ephemeral_port>
// that accepts an *unauthenticated* local SOCKS5 connection and relays it to
// the real proxy, performing the RFC 1929 auth on the upstream leg. The profile
// is then pointed at socks5://127.0.0.1:<local_port>, so no 407/auth dialog ever
// appears locally.
//
// All sockets live on a dedicated IO thread owned by this manager. The public
// API is safe to call from the UI thread.
class SocksBridge {
 public:
  static SocksBridge* GetInstance();

  SocksBridge(const SocksBridge&) = delete;
  SocksBridge& operator=(const SocksBridge&) = delete;

  // Starts (or, if one already exists for `proxy.id`, reuses) a localhost bridge
  // upstreaming to `proxy`. `callback` runs on the calling sequence with the
  // bound local port, or 0 on failure. Idempotent per proxy id.
  void StartBridge(const MontiProxy& proxy,
                   base::OnceCallback<void(int local_port)> callback);

  // Tears down the bridge for `proxy_id` (closes its listener and all live
  // connections). No-op if none exists.
  void StopBridge(const std::string& proxy_id);

 private:
  friend class base::NoDestructor<SocksBridge>;

  class BridgeInstance;
  class BridgeConnection;

  SocksBridge();
  ~SocksBridge();

  // IO-thread implementations.
  void StartOnIo(MontiProxy proxy,
                 scoped_refptr<base::SequencedTaskRunner> reply_runner,
                 base::OnceCallback<void(int)> callback);
  void StopOnIo(std::string proxy_id);

  base::Thread io_thread_;

  // Keyed by proxy id; only touched on the IO thread.
  std::map<std::string, std::unique_ptr<BridgeInstance>> instances_;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_SOCKS_BRIDGE_H_

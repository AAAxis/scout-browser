// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_PROXY_APPLICATOR_H_
#define CHROME_BROWSER_MONTI_PROXY_APPLICATOR_H_

#include "chrome/browser/monti/monti_proxy.h"

class Profile;

namespace monti {

// Routes `profile`'s traffic through `proxy` (HTTP port) by writing the
// per-profile proxy pref, and preemptively seeds the network context's auth
// cache with the proxy credentials so no 407 dialog appears.
void ApplyProxyToProfile(Profile* profile, const MontiProxy& proxy);

// Routes `profile` through an already-running local SOCKS5 bridge listening on
// 127.0.0.1:`local_port`. No HttpAuthCache seeding is needed: the upstream
// authentication is performed inside the bridge, so the local SOCKS5 hop is
// unauthenticated and never triggers a 407 dialog.
void ApplySocksProxyToProfile(Profile* profile, int local_port);

// Reverts `profile` to a direct (no proxy) connection.
void ClearProxyFromProfile(Profile* profile);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_PROXY_APPLICATOR_H_

// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_MONTI_MONTI_FINGERPRINT_INJECTOR_H_
#define CHROME_RENDERER_MONTI_MONTI_FINGERPRINT_INJECTOR_H_

#include <string>

namespace blink {
class WebLocalFrame;
}

namespace monti {

// Injects the per-profile fingerprint spoofing script into `frame`'s main world
// at document-start. `fingerprint_json` is the compact JSON produced by
// monti::ToJson on the browser side and shipped via RendererPreferences; an
// empty string is a no-op.
//
// The script overrides the JS-layer fingerprint surfaces (navigator hardware,
// screen, timezone, WebGL vendor/renderer, seeded canvas/audio noise) so the
// frame reports the profile's identity instead of the host's. It is idempotent:
// re-running it on the same window object is a no-op. navigator.userAgent itself
// is left to the session-14 UA override; this only spoofs the surfaces that
// must *agree* with that UA.
void InjectFingerprintSpoof(blink::WebLocalFrame* frame,
                            const std::string& fingerprint_json);

}  // namespace monti

#endif  // CHROME_RENDERER_MONTI_MONTI_FINGERPRINT_INJECTOR_H_

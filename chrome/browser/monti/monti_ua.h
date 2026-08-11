// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_UA_H_
#define CHROME_BROWSER_MONTI_MONTI_UA_H_

#include <optional>
#include <string>

#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"

namespace monti {

// Builds a coherent per-profile User-Agent + UA Client Hints identity from a
// stored preset key. This is the cheap, HTTP/JS-layer half of the fingerprint
// engine (session 14): the preset fixes a self-consistent platform string,
// UA-CH metadata, brand list and form-factor so a launched profile presents one
// browser identity with no mismatch tell.
//
// Values are deliberately host-independent: the "windows" preset reports Windows
// even when Monti runs on macOS. The Chrome major/full version, however, tracks
// the real build so the brand list stays truthful. Session 17 replaces these
// fixed presets with a full Generate() feeding the same UserAgentMetadata.
//
// Recognized presets: "windows", "macos", "linux". Anything else (notably ""
// and "system") means "no override" -> std::nullopt / empty preview.

// Returns the override for `preset`, or std::nullopt when no override applies.
std::optional<blink::UserAgentOverride> MontiUserAgentFor(
    const std::string& preset);

// Returns just the User-Agent string the preset would send (empty for the
// no-override presets), for the read-only preview in the Profiles edit drawer.
std::string MontiUaPreviewString(const std::string& preset);

// Returns the Sec-CH-UA-Platform label the preset reports ("Windows" | "macOS"
// | "Linux"), or "" for the no-override presets. Single source of truth for the
// platform token, reused by the fingerprint generator so the generated identity
// agrees with the User-Agent the launched profile sends.
std::string MontiPlatformLabelFor(const std::string& preset);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_UA_H_

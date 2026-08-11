// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PREF_NAMES_H_
#define CHROME_BROWSER_MONTI_MONTI_PREF_NAMES_H_

namespace monti::prefs {

// Bool (default true): when set, a launched profile's new-tab "+" opens the
// branded chrome://monti-newtab page instead of Google's NTP. Defined locally
// (not in chrome/common/pref_names.h) to keep that hot header untouched.
inline constexpr char kMontiUseMontiNewTab[] = "monti.use_monti_newtab";

// List of {title, url, icon} dicts: the per-profile favorites grid shown on
// chrome://monti-newtab. Per-profile (lives in the profile's own prefs), so it
// is isolated per profile and survives restart for free.
inline constexpr char kMontiFavorites[] = "monti.favorites";

// --- chrome://monti-settings (session 34) ---
// These back the global settings surface. They are registered per-profile (in
// MontiProfileService::RegisterProfilePrefs) and read by the settings handler
// plus MontiManager::CreateProfile when seeding a fresh fingerprint.

// String: default fingerprint preset for newly created profiles —
// "windows" | "macos" | "linux" | "" (empty == let Generate() pick).
inline constexpr char kMontiDefaultFingerprintPreset[] =
    "monti.default_fingerprint_preset";

// Dict of per-surface spoof modes applied to new profiles. Keys: "webrtc",
// "canvas", "webgl", "webgpu", "client_rects", "audio". Values: "real" |
// "noise" | "off". Missing keys keep the generator default ("real").
inline constexpr char kMontiDefaultSpoofModes[] = "monti.default_spoof_modes";

// Bool (default false): new profiles set their timezone from the assigned
// proxy's egress geolocation instead of the generated one.
inline constexpr char kMontiTimezoneFollowsProxy[] =
    "monti.timezone_follows_proxy";

// Bool (default true): new profiles force WebRTC into a leak-protected mode.
inline constexpr char kMontiWebrtcLeakProtection[] =
    "monti.webrtc_leak_protection";

// String: app theme — "light" | "dark" | "system" (default "system").
inline constexpr char kMontiTheme[] = "monti.theme";

// String: default search engine keyword/host for new profiles ("" == browser
// default).
inline constexpr char kMontiDefaultSearchEngine[] =
    "monti.default_search_engine";

// Bools (default true): desktop notification toggles.
inline constexpr char kMontiNotifyBans[] = "monti.notify_bans";
inline constexpr char kMontiNotifyProxyDown[] = "monti.notify_proxy_down";
inline constexpr char kMontiNotifySync[] = "monti.notify_sync";

}  // namespace monti::prefs

#endif  // CHROME_BROWSER_MONTI_MONTI_PREF_NAMES_H_

// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_FINGERPRINT_H_
#define CHROME_BROWSER_MONTI_MONTI_FINGERPRINT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"

namespace base {
class CommandLine;
}  // namespace base

namespace monti {

// The complete per-profile anti-detect fingerprint. Mirrors every field the
// session-20 Advanced editor exposes, plus the coherent hardware identity the
// New-profile Summary card renders. Plain data; persisted by ProfileStore (in
// the profile's registry record) and mirrored into the loaded profile's prefs
// via MontiProfileService so the renderer/launch enforcement of session 17 can
// read it back.
//
// Two halves:
//  - *Generated* hardware identity (platform/ua_string/GPU/screen/CPU/memory/
//    timezone/languages/fonts/...) -- produced by Generate() and re-rolled by a
//    "New fingerprint" action.
//  - *Config* (the spoof-mode enums, ports, DNT, switches, flags and Notes) --
//    left at defaults by Generate(); the user owns these and a re-roll must
//    preserve them (the manager merges).
struct Fingerprint {
  // Bookkeeping: identity coherent with the session-14 UA preset.
  std::string platform;    // "Windows" | "macOS" | "Linux"; "" == no override
  std::string ua_string;   // == MontiUaPreviewString(preset)
  std::string preset;      // "windows" | "macos" | "linux" | ""
  uint32_t seed = 0;       // the seed Generate() was keyed by

  // Spoof modes: "real" | "noise" | "off" | "manual". Config (default "real").
  std::string webrtc_mode = "noise";
  std::string canvas_mode = "noise";
  std::string webgl_mode = "noise";
  std::string webgpu_mode = "real";
  std::string client_rects_mode = "noise";
  std::string audio_mode = "noise";
  // WebGL Info -- generated, coherent with the platform.
  std::string webgl_vendor;
  std::string webgl_renderer;

  // General.
  std::string timezone;                  // generated, e.g. "America/Los_Angeles"
  std::vector<std::string> languages;    // generated, e.g. {"en-US", "en"}
  std::string geolocation_mode = "real";  // config
  double latitude = 0;                   // config
  double longitude = 0;                  // config

  // Additional hardware -- generated.
  int cpu_cores = 0;   // navigator.hardwareConcurrency
  int memory_gb = 0;   // navigator.deviceMemory
  std::string mac_address;
  std::string device_name;
  std::vector<std::string> fonts;

  // Screen & media -- generated.
  std::string screen;         // e.g. "1512 × 982 · 30-bit"
  std::string media_devices;  // human summary, e.g. "1 cam · 1 mic · 1 speaker"

  // Extra -- config.
  std::string ports_to_protect =
      "3389,5900,5800,7070,6568,5938,63333,5901,5902,5903,5950,5931,5939,6039,"
      "5944,6040,5279,2112";
  bool do_not_track = false;
  std::string command_line_switches;
  bool hide_profile_name = false;
  bool video_cookie_spoof = false;
  bool substitute_name_icon = false;

  // Notes -- config.
  std::string note_text;
  std::string note_icon;
  std::string note_color;
  std::string note_style;
};

// Produces a coherent fingerprint for `preset` ("windows" | "macos" | "linux"),
// deterministic in `seed`: the same (preset, seed) always yields identical
// values, while a different seed yields different-but-still-coherent values.
// platform/ua_string are taken from the session-14 source of truth so the
// generated identity agrees with the launched profile's User-Agent. Config
// fields (modes/ports/DNT/switches/flags/Notes) are left at their defaults so
// re-rolling never wipes a user's chosen configuration.
//
// Callers supply `seed` (the browser may seed fresh generations from
// base::RandUint64()); this function performs no nondeterministic I/O.
Fingerprint Generate(const std::string& preset, uint32_t seed);

// Applies country-level locale defaults from a detected proxy egress country.
// This keeps language, timezone, and coarse geolocation telling the same story
// as the proxy. Unknown/empty countries leave `fp` unchanged.
Fingerprint ApplyCountryDefaults(Fingerprint fp, const std::string& country);

// Round-trips the struct through a base::Value::Dict for the mojom mapper and
// the MontiProfileService prefs mirror. FromDict is additive/defaulted: a
// partial or empty dict yields a default-constructed Fingerprint with whatever
// keys were present applied on top.
base::DictValue ToDict(const Fingerprint& fp);
Fingerprint FromDict(const base::DictValue& dict);

// Round-trips the struct through a compact JSON string for the ProfileStore
// registry record (which must stay trivially copyable). FromJson returns a
// default-constructed Fingerprint for an empty or malformed string.
std::string ToJson(const Fingerprint& fp);
Fingerprint FromJson(const std::string& json);

// Decodes and validates the --monti-fingerprint-json switch an external
// launcher (Monti Anty) passes for a --monti-profile-launch session: the
// switch value is base64url(JSON) matching ToJson()/FromJson()'s shape.
// Returns nullopt if the switch is absent, not valid base64, or does not
// decode to a valid JSON object -- callers must never forward the raw
// switch value anywhere it could be interpreted as script (it is decoded
// here and always re-derived via ToJson()/ToDict() from here on, so a
// malformed value can never smuggle syntax into the renderer's
// `var FP=<json>;` injection).
std::optional<Fingerprint> DecodeRuntimeFingerprintSwitch(
    const base::CommandLine& command_line);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_FINGERPRINT_H_

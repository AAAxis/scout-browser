// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_fingerprint.h"

#include <array>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/monti/monti_ua.h"

namespace monti {

namespace {

// A coherent hardware pool for one platform. Everything Generate() draws from is
// indexed off the seeded RNG so a given (preset, seed) is fully reproducible.
struct PlatformPool {
  // Parallel vendor/renderer pairs for the WebGL "Unmasked" strings.
  std::vector<std::pair<const char*, const char*>> webgl;
  std::vector<const char*> screens;       // "W × H · depth"
  std::vector<const char*> timezones;     // IANA zone ids
  std::vector<const char*> languages;     // primary language tag (e.g. "en-US")
  std::vector<const char*> fonts;         // plausible installed-font sample
  const char* device_prefix;              // device_name stem
};

const PlatformPool& MacPool() {
  static const base::NoDestructor<PlatformPool> pool(PlatformPool{
      .webgl =
          {
              {"Apple Inc.", "Apple M1"},
              {"Apple Inc.", "Apple M2"},
              {"Apple Inc.", "Apple M3"},
              {"Google Inc. (Apple)",
               "ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified "
               "Version)"},
              {"Google Inc. (Intel Inc.)",
               "ANGLE (Intel Inc., Intel(R) Iris(TM) Plus Graphics 655, OpenGL "
               "4.1)"},
              {"Google Inc. (Intel Inc.)",
               "ANGLE (Intel Inc., Intel(R) UHD Graphics 630, OpenGL 4.1)"},
          },
      .screens =
          {
              "1512 × 982 · 30-bit",   // 14" MacBook Pro (scaled)
              "1728 × 1117 · 30-bit",  // 16" MacBook Pro (scaled)
              "1440 × 900 · 30-bit",   // MacBook Air
              "1680 × 1050 · 30-bit",
              "2560 × 1440 · 30-bit",  // Studio Display (scaled)
          },
      .timezones =
          {
              "America/Los_Angeles",
              "America/New_York",
              "America/Chicago",
              "Europe/London",
              "Europe/Berlin",
          },
      .languages = {"en-US", "en-GB", "fr-FR", "de-DE", "es-ES"},
      .fonts =
          {
              "Helvetica Neue", "Helvetica", "Geneva", "Menlo", "Monaco",
              "Lucida Grande", "Apple Color Emoji", "Avenir", "Optima",
              "Times",
          },
      .device_prefix = "MacBook-Pro",
  });
  return *pool;
}

const PlatformPool& WindowsPool() {
  static const base::NoDestructor<PlatformPool> pool(PlatformPool{
      .webgl =
          {
              {"Google Inc. (NVIDIA)",
               "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 "
               "ps_5_0, D3D11)"},
              {"Google Inc. (NVIDIA)",
               "ANGLE (NVIDIA, NVIDIA GeForce GTX 1660 Ti Direct3D11 vs_5_0 "
               "ps_5_0, D3D11)"},
              {"Google Inc. (Intel)",
               "ANGLE (Intel, Intel(R) UHD Graphics 770 Direct3D11 vs_5_0 "
               "ps_5_0, D3D11)"},
              {"Google Inc. (AMD)",
               "ANGLE (AMD, AMD Radeon RX 6600 Direct3D11 vs_5_0 ps_5_0, "
               "D3D11)"},
          },
      .screens =
          {
              "1920 × 1080 · 24-bit",
              "2560 × 1440 · 24-bit",
              "1366 × 768 · 24-bit",
              "1536 × 864 · 24-bit",
              "3840 × 2160 · 24-bit",
          },
      .timezones =
          {
              "America/New_York",
              "America/Chicago",
              "Europe/London",
              "Europe/Berlin",
              "Europe/Moscow",
          },
      .languages = {"en-US", "en-GB", "ru-RU", "de-DE", "pt-BR"},
      .fonts =
          {
              "Arial", "Calibri", "Cambria", "Consolas", "Segoe UI",
              "Tahoma", "Times New Roman", "Verdana", "Georgia",
              "Trebuchet MS",
          },
      .device_prefix = "DESKTOP",
  });
  return *pool;
}

const PlatformPool& LinuxPool() {
  static const base::NoDestructor<PlatformPool> pool(PlatformPool{
      .webgl =
          {
              {"Google Inc. (Intel)",
               "ANGLE (Intel, Mesa Intel(R) UHD Graphics (CML GT2), OpenGL "
               "4.6)"},
              {"Google Inc. (AMD)",
               "ANGLE (AMD, AMD Radeon Graphics (radeonsi, renoir, LLVM 15.0.6, "
               "DRM 3.49), OpenGL 4.6)"},
              {"Mesa", "llvmpipe (LLVM 15.0.6, 256 bits)"},
          },
      .screens =
          {
              "1920 × 1080 · 24-bit",
              "2560 × 1440 · 24-bit",
              "1680 × 1050 · 24-bit",
              "1600 × 900 · 24-bit",
          },
      .timezones =
          {
              "Europe/Berlin",
              "Europe/Paris",
              "America/New_York",
              "Etc/UTC",
          },
      .languages = {"en-US", "de-DE", "fr-FR", "en-GB"},
      .fonts =
          {
              "DejaVu Sans", "DejaVu Serif", "Liberation Sans",
              "Liberation Serif", "Noto Sans", "Ubuntu", "Cantarell",
              "FreeSans", "Droid Sans",
          },
      .device_prefix = "ubuntu",
  });
  return *pool;
}

const PlatformPool& PoolFor(const std::string& preset) {
  if (preset == "windows") {
    return WindowsPool();
  }
  if (preset == "linux") {
    return LinuxPool();
  }
  // Default to the macOS pool for "macos" and any unknown preset, so a
  // fingerprint is always coherent rather than empty.
  return MacPool();
}

// Returns a reference to a deterministically-picked element of `pool`.
template <typename T>
const T& Pick(std::mt19937& rng, const std::vector<T>& pool) {
  return pool[rng() % pool.size()];
}

// Synthesizes a plausible, locally-administered MAC address from the RNG.
std::string MakeMacAddress(std::mt19937& rng) {
  std::array<unsigned, 6> octets;
  for (unsigned& octet : octets) {
    octet = static_cast<unsigned>(rng() & 0xFF);
  }
  // Set the locally-administered bit and clear the multicast bit on the first
  // octet, the way a software-assigned unicast MAC looks.
  octets[0] = (octets[0] & 0xFEu) | 0x02u;
  return base::StringPrintf("%02X:%02X:%02X:%02X:%02X:%02X", octets[0],
                            octets[1], octets[2], octets[3], octets[4],
                            octets[5]);
}

struct CountryDefaults {
  const char* country;
  const char* timezone;
  const char* language;
  double latitude;
  double longitude;
};

const CountryDefaults* DefaultsForCountry(const std::string& country) {
  const std::string code = base::ToLowerASCII(country);
  static constexpr CountryDefaults kDefaults[] = {
      {"us", "America/New_York", "en-US", 40.7128, -74.0060},
      {"ca", "America/Toronto", "en-CA", 43.6532, -79.3832},
      {"gb", "Europe/London", "en-GB", 51.5074, -0.1278},
      {"ie", "Europe/Dublin", "en-IE", 53.3498, -6.2603},
      {"au", "Australia/Sydney", "en-AU", -33.8688, 151.2093},
      {"nz", "Pacific/Auckland", "en-NZ", -36.8485, 174.7633},
      {"es", "Europe/Madrid", "es-ES", 40.4168, -3.7038},
      {"mx", "America/Mexico_City", "es-MX", 19.4326, -99.1332},
      {"ar", "America/Argentina/Buenos_Aires", "es-AR", -34.6037, -58.3816},
      {"co", "America/Bogota", "es-CO", 4.7110, -74.0721},
      {"br", "America/Sao_Paulo", "pt-BR", -23.5558, -46.6396},
      {"pt", "Europe/Lisbon", "pt-PT", 38.7223, -9.1393},
      {"fr", "Europe/Paris", "fr-FR", 48.8566, 2.3522},
      {"de", "Europe/Berlin", "de-DE", 52.5200, 13.4050},
      {"at", "Europe/Vienna", "de-AT", 48.2082, 16.3738},
      {"ch", "Europe/Zurich", "de-CH", 47.3769, 8.5417},
      {"nl", "Europe/Amsterdam", "nl-NL", 52.3676, 4.9041},
      {"be", "Europe/Brussels", "nl-BE", 50.8503, 4.3517},
      {"it", "Europe/Rome", "it-IT", 41.9028, 12.4964},
      {"pl", "Europe/Warsaw", "pl-PL", 52.2297, 21.0122},
      {"cz", "Europe/Prague", "cs-CZ", 50.0755, 14.4378},
      {"se", "Europe/Stockholm", "sv-SE", 59.3293, 18.0686},
      {"no", "Europe/Oslo", "nb-NO", 59.9139, 10.7522},
      {"dk", "Europe/Copenhagen", "da-DK", 55.6761, 12.5683},
      {"fi", "Europe/Helsinki", "fi-FI", 60.1699, 24.9384},
      {"ru", "Europe/Moscow", "ru-RU", 55.7558, 37.6173},
      {"ua", "Europe/Kyiv", "uk-UA", 50.4501, 30.5234},
      {"tr", "Europe/Istanbul", "tr-TR", 41.0082, 28.9784},
      {"il", "Asia/Jerusalem", "he-IL", 31.7683, 35.2137},
      {"ae", "Asia/Dubai", "ar-AE", 25.2048, 55.2708},
      {"in", "Asia/Kolkata", "en-IN", 28.6139, 77.2090},
      {"sg", "Asia/Singapore", "en-SG", 1.3521, 103.8198},
      {"jp", "Asia/Tokyo", "ja-JP", 35.6762, 139.6503},
      {"kr", "Asia/Seoul", "ko-KR", 37.5665, 126.9780},
      {"hk", "Asia/Hong_Kong", "zh-HK", 22.3193, 114.1694},
      {"tw", "Asia/Taipei", "zh-TW", 25.0330, 121.5654},
      {"th", "Asia/Bangkok", "th-TH", 13.7563, 100.5018},
      {"vn", "Asia/Ho_Chi_Minh", "vi-VN", 10.8231, 106.6297},
      {"id", "Asia/Jakarta", "id-ID", -6.2088, 106.8456},
      {"ph", "Asia/Manila", "en-PH", 14.5995, 120.9842},
      {"za", "Africa/Johannesburg", "en-ZA", -26.2041, 28.0473},
  };
  for (const CountryDefaults& item : kDefaults) {
    if (code == item.country) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace

Fingerprint Generate(const std::string& preset, uint32_t seed) {
  Fingerprint fp;
  fp.preset = preset;
  fp.seed = seed;
  // Identity coherent with the session-14 UA preset (single source of truth).
  fp.platform = MontiPlatformLabelFor(preset);
  fp.ua_string = MontiUaPreviewString(preset);

  std::mt19937 rng(seed);
  const PlatformPool& pool = PoolFor(preset);

  const auto& webgl = Pick(rng, pool.webgl);
  fp.webgl_vendor = webgl.first;
  fp.webgl_renderer = webgl.second;

  fp.screen = Pick(rng, pool.screens);
  fp.timezone = Pick(rng, pool.timezones);

  // Primary language plus its bare base tag, e.g. {"en-US", "en"}.
  std::string primary = Pick(rng, pool.languages);
  fp.languages.push_back(primary);
  std::string base_tag = primary.substr(0, primary.find('-'));
  if (!base_tag.empty() && base_tag != primary) {
    fp.languages.push_back(base_tag);
  }

  static constexpr std::array<int, 4> kCores = {4, 8, 12, 16};
  static constexpr std::array<int, 3> kMemory = {8, 16, 32};
  fp.cpu_cores = kCores[rng() % kCores.size()];
  fp.memory_gb = kMemory[rng() % kMemory.size()];

  fp.mac_address = MakeMacAddress(rng);
  fp.device_name = base::StringPrintf("%s-%04X", pool.device_prefix,
                                      static_cast<unsigned>(rng() & 0xFFFF));

  // A coherent random sample of the platform's fonts (about two thirds).
  for (const char* font : pool.fonts) {
    if (rng() % 3 != 0) {
      fp.fonts.push_back(font);
    }
  }
  if (fp.fonts.empty()) {
    fp.fonts.push_back(pool.fonts.front());
  }

  fp.media_devices = "1 camera · 1 microphone · 1 speaker";

  // Config fields (modes/ports/DNT/switches/flags/Notes) intentionally stay at
  // their struct defaults; a re-roll must not overwrite the user's choices.
  return fp;
}

Fingerprint ApplyCountryDefaults(Fingerprint fp, const std::string& country) {
  const CountryDefaults* defaults = DefaultsForCountry(country);
  if (!defaults) {
    return fp;
  }
  fp.timezone = defaults->timezone;
  fp.languages.clear();
  fp.languages.push_back(defaults->language);
  std::string base_tag = fp.languages.front().substr(
      0, fp.languages.front().find('-'));
  if (!base_tag.empty() && base_tag != fp.languages.front()) {
    fp.languages.push_back(base_tag);
  }
  fp.geolocation_mode = "manual";
  fp.latitude = defaults->latitude;
  fp.longitude = defaults->longitude;
  return fp;
}

base::DictValue ToDict(const Fingerprint& fp) {
  base::DictValue dict;
  dict.Set("platform", fp.platform);
  dict.Set("ua_string", fp.ua_string);
  dict.Set("preset", fp.preset);
  dict.Set("seed", static_cast<double>(fp.seed));

  dict.Set("webrtc_mode", fp.webrtc_mode);
  dict.Set("canvas_mode", fp.canvas_mode);
  dict.Set("webgl_mode", fp.webgl_mode);
  dict.Set("webgpu_mode", fp.webgpu_mode);
  dict.Set("client_rects_mode", fp.client_rects_mode);
  dict.Set("audio_mode", fp.audio_mode);
  dict.Set("webgl_vendor", fp.webgl_vendor);
  dict.Set("webgl_renderer", fp.webgl_renderer);

  dict.Set("timezone", fp.timezone);
  base::ListValue languages;
  for (const std::string& lang : fp.languages) {
    languages.Append(lang);
  }
  dict.Set("languages", std::move(languages));
  dict.Set("geolocation_mode", fp.geolocation_mode);
  dict.Set("latitude", fp.latitude);
  dict.Set("longitude", fp.longitude);

  dict.Set("cpu_cores", fp.cpu_cores);
  dict.Set("memory_gb", fp.memory_gb);
  dict.Set("mac_address", fp.mac_address);
  dict.Set("device_name", fp.device_name);
  base::ListValue fonts;
  for (const std::string& font : fp.fonts) {
    fonts.Append(font);
  }
  dict.Set("fonts", std::move(fonts));

  dict.Set("screen", fp.screen);
  dict.Set("media_devices", fp.media_devices);

  dict.Set("ports_to_protect", fp.ports_to_protect);
  dict.Set("do_not_track", fp.do_not_track);
  dict.Set("command_line_switches", fp.command_line_switches);
  dict.Set("hide_profile_name", fp.hide_profile_name);
  dict.Set("video_cookie_spoof", fp.video_cookie_spoof);
  dict.Set("substitute_name_icon", fp.substitute_name_icon);

  dict.Set("note_text", fp.note_text);
  dict.Set("note_icon", fp.note_icon);
  dict.Set("note_color", fp.note_color);
  dict.Set("note_style", fp.note_style);
  return dict;
}

Fingerprint FromDict(const base::DictValue& dict) {
  Fingerprint fp;

  auto read_string = [&dict](const char* key, std::string& out) {
    if (const std::string* v = dict.FindString(key)) {
      out = *v;
    }
  };

  read_string("platform", fp.platform);
  read_string("ua_string", fp.ua_string);
  read_string("preset", fp.preset);
  if (std::optional<double> v = dict.FindDouble("seed")) {
    fp.seed = static_cast<uint32_t>(*v);
  }

  read_string("webrtc_mode", fp.webrtc_mode);
  read_string("canvas_mode", fp.canvas_mode);
  read_string("webgl_mode", fp.webgl_mode);
  read_string("webgpu_mode", fp.webgpu_mode);
  read_string("client_rects_mode", fp.client_rects_mode);
  read_string("audio_mode", fp.audio_mode);
  read_string("webgl_vendor", fp.webgl_vendor);
  read_string("webgl_renderer", fp.webgl_renderer);

  read_string("timezone", fp.timezone);
  if (const base::ListValue* languages = dict.FindList("languages")) {
    fp.languages.clear();
    for (const base::Value& lang : *languages) {
      if (lang.is_string()) {
        fp.languages.push_back(lang.GetString());
      }
    }
  }
  read_string("geolocation_mode", fp.geolocation_mode);
  fp.latitude = dict.FindDouble("latitude").value_or(fp.latitude);
  fp.longitude = dict.FindDouble("longitude").value_or(fp.longitude);

  fp.cpu_cores = dict.FindInt("cpu_cores").value_or(fp.cpu_cores);
  fp.memory_gb = dict.FindInt("memory_gb").value_or(fp.memory_gb);
  read_string("mac_address", fp.mac_address);
  read_string("device_name", fp.device_name);
  if (const base::ListValue* fonts = dict.FindList("fonts")) {
    fp.fonts.clear();
    for (const base::Value& font : *fonts) {
      if (font.is_string()) {
        fp.fonts.push_back(font.GetString());
      }
    }
  }

  read_string("screen", fp.screen);
  read_string("media_devices", fp.media_devices);

  read_string("ports_to_protect", fp.ports_to_protect);
  fp.do_not_track = dict.FindBool("do_not_track").value_or(fp.do_not_track);
  read_string("command_line_switches", fp.command_line_switches);
  fp.hide_profile_name =
      dict.FindBool("hide_profile_name").value_or(fp.hide_profile_name);
  fp.video_cookie_spoof =
      dict.FindBool("video_cookie_spoof").value_or(fp.video_cookie_spoof);
  fp.substitute_name_icon =
      dict.FindBool("substitute_name_icon").value_or(fp.substitute_name_icon);

  read_string("note_text", fp.note_text);
  read_string("note_icon", fp.note_icon);
  read_string("note_color", fp.note_color);
  read_string("note_style", fp.note_style);
  return fp;
}

std::string ToJson(const Fingerprint& fp) {
  std::optional<std::string> json = base::WriteJson(ToDict(fp));
  return json.value_or(std::string());
}

Fingerprint FromJson(const std::string& json) {
  if (json.empty()) {
    return Fingerprint();
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  return dict ? FromDict(*dict) : Fingerprint();
}

std::optional<Fingerprint> DecodeRuntimeFingerprintSwitch(
    const base::CommandLine& command_line) {
  constexpr char kSwitch[] = "monti-fingerprint-json";
  if (!command_line.HasSwitch(kSwitch)) {
    return std::nullopt;
  }
  // base64url -> standard base64 (the launcher never emits padding, so restore
  // it before decoding).
  std::string base64 = command_line.GetSwitchValueASCII(kSwitch);
  base::ReplaceChars(base64, "-", "+", &base64);
  base::ReplaceChars(base64, "_", "/", &base64);
  while (base64.size() % 4 != 0) {
    base64.push_back('=');
  }
  std::string json;
  if (!base::Base64Decode(base64, &json)) {
    return std::nullopt;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  return FromDict(*dict);
}

}  // namespace monti

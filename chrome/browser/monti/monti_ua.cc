// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_ua.h"

#include "components/embedder_support/user_agent_utils.h"
#include "components/version_info/version_info.h"

namespace monti {

namespace {

// The fixed, host-independent fields that distinguish one preset's platform
// identity. Everything else (brand list, Chrome version) is shared and tracks
// the real build so the brands stay truthful.
struct Preset {
  // The OS token spliced into the UA string's "(...)" section, e.g.
  // "Windows NT 10.0; Win64; x64".
  const char* os_info;
  // Sec-CH-UA-Platform (no quotes): "Windows" | "macOS" | "Linux".
  const char* platform;
  // Sec-CH-UA-Platform-Version. Empty for Linux, which Chrome leaves blank.
  const char* platform_version;
};

// Resolves a preset key to its fixed fields. Returns nullptr for the
// no-override presets ("" / "system" / unknown).
const Preset* LookupPreset(const std::string& preset) {
  static constexpr Preset kWindows = {"Windows NT 10.0; Win64; x64", "Windows",
                                      "10.0.0"};
  static constexpr Preset kMac = {"Macintosh; Intel Mac OS X 10_15_7", "macOS",
                                  "13.0.0"};
  static constexpr Preset kLinux = {"X11; Linux x86_64", "Linux", ""};
  if (preset == "windows") {
    return &kWindows;
  }
  if (preset == "macos") {
    return &kMac;
  }
  if (preset == "linux") {
    return &kLinux;
  }
  return nullptr;
}

// Builds the UA string for a preset. Reuses Chrome's own assembler so the
// AppleWebKit/Safari scaffolding and Chrome version match a real Chrome exactly;
// only the OS token is ours.
std::string BuildUaString(const Preset& preset) {
  return embedder_support::BuildUserAgentFromOSAndProduct(
      preset.os_info, embedder_support::GetProductAndVersion());
}

}  // namespace

std::optional<blink::UserAgentOverride> MontiUserAgentFor(
    const std::string& preset) {
  const Preset* p = LookupPreset(preset);
  if (!p) {
    return std::nullopt;
  }

  blink::UserAgentOverride override;
  override.ua_string_override = BuildUaString(*p);

  blink::UserAgentMetadata metadata;
  // Brand lists and Chrome version come from the real build (host-independent,
  // and identical to what an unspoofed Chrome of this version would send).
  metadata.brand_version_list =
      embedder_support::GetUserAgentBrandMajorVersionList();
  metadata.brand_full_version_list =
      embedder_support::GetUserAgentBrandFullVersionList();
  metadata.full_version = std::string(version_info::GetVersionNumber());
  // Platform identity is fixed by the preset.
  metadata.platform = p->platform;
  metadata.platform_version = p->platform_version;
  metadata.architecture = "x86";
  metadata.bitness = "64";
  metadata.model = std::string();
  metadata.mobile = false;
  metadata.wow64 = false;
  metadata.form_factors = {blink::kDesktopFormFactor};

  override.ua_metadata_override = std::move(metadata);
  return override;
}

std::string MontiUaPreviewString(const std::string& preset) {
  const Preset* p = LookupPreset(preset);
  return p ? BuildUaString(*p) : std::string();
}

std::string MontiPlatformLabelFor(const std::string& preset) {
  const Preset* p = LookupPreset(preset);
  return p ? std::string(p->platform) : std::string();
}

}  // namespace monti

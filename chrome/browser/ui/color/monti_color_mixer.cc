// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/color/monti_color_mixer.h"

#include "chrome/browser/ui/color/chrome_color_id.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_recipe.h"

void AddMontiColorMixer(ui::ColorProvider* provider,
                        const ui::ColorProviderKey& key) {
  // Monti uses Chromium's existing dark palette as-is; only the light-mode
  // toolbar/bookmark rows take the warm off-white. Skip when a custom theme is
  // active so user themes keep control of these surfaces.
  const bool dark_mode =
      key.color_mode == ui::ColorProviderKey::ColorMode::kDark;
  if (dark_mode || key.custom_theme) {
    return;
  }

  // Warm off-white shared by the URL (toolbar) row and the bookmarks bar.
  constexpr SkColor kMontiRow = SkColorSetRGB(0xFD, 0xFC, 0xF8);

  ui::ColorMixer& mixer = provider->AddMixer();
  mixer[kColorToolbar] = {kMontiRow};
  mixer[kColorBookmarkBarBackground] = {kMontiRow};

  // Make the omnibox/location-bar field blend into the toolbar row instead of
  // showing the default lighter-grey pill. Scoped to the location bar only, so
  // the broader kColorToolbarBackgroundSubtleEmphasis token (used by other
  // surfaces) is left untouched.
  mixer[kColorLocationBarBackground] = {kMontiRow};
  mixer[kColorLocationBarBackgroundHovered] = {kMontiRow};

  // When the omnibox is focused/clicked, the field and the suggestions popup
  // both switch to kColorOmniboxResultsBackground (see LocationBarView and
  // RoundedOmniboxResultsFrame), which defaults to pure white. Match it to the
  // row color so focusing the URL and the dropdown stay the same warm off-white.
  // The hovered/selected suggestion rows derive from this token, so they keep a
  // subtle contrast against the cream automatically.
  mixer[kColorOmniboxResultsBackground] = {kMontiRow};
}

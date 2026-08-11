// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_COLOR_MONTI_COLOR_MIXER_H_
#define CHROME_BROWSER_UI_COLOR_MONTI_COLOR_MIXER_H_

#include "ui/color/color_provider_key.h"

namespace ui {
class ColorProvider;
}

// Adds a color mixer that applies the Monti warm-neutral toolbar palette to
// |provider| with |key|. Currently overrides the toolbar and bookmark-bar row
// backgrounds to a warm off-white in light mode only.
void AddMontiColorMixer(ui::ColorProvider* provider,
                        const ui::ColorProviderKey& key);

#endif  // CHROME_BROWSER_UI_COLOR_MONTI_COLOR_MIXER_H_

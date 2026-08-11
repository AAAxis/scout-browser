// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_ua_tab_helper.h"

#include <utility>

#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

namespace monti {

// Per-WebContents arm of the helper: seeds the UA override + fingerprint on
// construction and re-asserts the UA on every primary main-frame navigation so
// browser-initiated loads (omnibox) keep the spoofed identity. The fingerprint
// JSON and WebRTC policy ride on RendererPreferences, which propagate to every
// (sub)frame the WebContents creates.
class MontiUaTabHelper::TabObserver : public content::WebContentsObserver {
 public:
  TabObserver(content::WebContents* web_contents,
              const std::optional<blink::UserAgentOverride>& ua_override,
              const std::string& fingerprint_json,
              blink::mojom::WebRtcIpHandlingPolicy webrtc_policy)
      : content::WebContentsObserver(web_contents),
        override_ua_(ua_override.has_value()) {
    if (ua_override) {
      web_contents->SetUserAgentOverride(*ua_override,
                                         /*override_in_new_tabs=*/true);
    }
    if (!fingerprint_json.empty() ||
        webrtc_policy != blink::mojom::WebRtcIpHandlingPolicy::kDefault) {
      blink::RendererPreferences* prefs =
          web_contents->GetMutableRendererPrefs();
      prefs->monti_fingerprint_json = fingerprint_json;
      prefs->webrtc_ip_handling_policy = webrtc_policy;
      web_contents->SyncRendererPrefs();
    }
  }

  // content::WebContentsObserver:
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override {
    if (!override_ua_ || !navigation_handle->IsInPrimaryMainFrame() ||
        navigation_handle->IsSameDocument()) {
      return;
    }
    navigation_handle->SetIsOverridingUserAgent(true);
  }

 private:
  const bool override_ua_;
};

MontiUaTabHelper::MontiUaTabHelper(
    TabStripModel* tab_strip_model,
    std::optional<blink::UserAgentOverride> ua_override,
    std::string fingerprint_json,
    blink::mojom::WebRtcIpHandlingPolicy webrtc_policy)
    : ua_override_(std::move(ua_override)),
      fingerprint_json_(std::move(fingerprint_json)),
      webrtc_policy_(webrtc_policy),
      tab_strip_model_(tab_strip_model) {
  tab_strip_model_->AddObserver(this);
  // Cover tabs that already exist when the window opens (e.g. the initial NTP).
  for (int i = 0; i < tab_strip_model_->count(); ++i) {
    StartObserving(tab_strip_model_->GetWebContentsAt(i));
  }
}

// The base TabStripModelObserver destructor removes us from any model we are
// still observing, so no explicit RemoveObserver is needed here.
MontiUaTabHelper::~MontiUaTabHelper() = default;

void MontiUaTabHelper::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  switch (change.type()) {
    case TabStripModelChange::kInserted:
      for (const auto& entry : change.GetInsert()->contents) {
        StartObserving(entry.contents);
      }
      break;
    case TabStripModelChange::kRemoved:
      for (const auto& entry : change.GetRemove()->contents) {
        StopObserving(entry.contents);
      }
      break;
    case TabStripModelChange::kReplaced: {
      const TabStripModelChange::Replace* replace = change.GetReplace();
      StopObserving(replace->old_contents);
      StartObserving(replace->new_contents);
      break;
    }
    case TabStripModelChange::kMoved:
    case TabStripModelChange::kSelectionOnly:
      break;
  }
}

void MontiUaTabHelper::OnTabStripModelDestroyed(
    TabStripModel* tab_strip_model) {
  // The framework has already removed us from the model before this call. The
  // window is gone, so the override no longer has anywhere to apply.
  delete this;
}

void MontiUaTabHelper::StartObserving(content::WebContents* web_contents) {
  if (!web_contents || tab_observers_.count(web_contents)) {
    return;
  }
  tab_observers_[web_contents] = std::make_unique<TabObserver>(
      web_contents, ua_override_, fingerprint_json_, webrtc_policy_);
}

void MontiUaTabHelper::StopObserving(content::WebContents* web_contents) {
  tab_observers_.erase(web_contents);
}

}  // namespace monti

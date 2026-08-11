// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_UA_TAB_HELPER_H_
#define CHROME_BROWSER_MONTI_MONTI_UA_TAB_HELPER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/peerconnection/webrtc_ip_handling_policy.mojom.h"

class TabStripModel;

namespace content {
class WebContents;
}

namespace monti {

// Keeps a per-profile anti-detect identity applied across an entire browser
// window: the session-14 User-Agent / UA-CH override *and* the session-17
// fingerprint that the renderer injects at document-start.
//
// SetUserAgentOverride is per-WebContents and, on its own, only reaches a tab's
// first renderer-initiated navigation; browser-initiated navigations (the
// omnibox) would otherwise revert. Likewise the fingerprint JSON has to ride on
// each tab's RendererPreferences before it navigates. This helper closes both
// gaps: it watches the window's TabStripModel so every tab (current and newly
// inserted) gets the UA override, the fingerprint JSON, and the WebRTC IP
// policy, and on each navigation forces
// NavigationHandle::SetIsOverridingUserAgent(true) so the identity holds
// regardless of who initiated it.
//
// Self-owned: created by MontiManager when an Monti-profile window opens with
// either an override-producing UA preset or a non-empty fingerprint, and
// deletes itself when the TabStripModel is destroyed (i.e. the window closes).
class MontiUaTabHelper : public TabStripModelObserver {
 public:
  // `ua_override` is nullopt for host-UA profiles; `fingerprint_json` is "" when
  // the profile has no fingerprint. At least one of them is meaningful when this
  // helper is created.
  MontiUaTabHelper(TabStripModel* tab_strip_model,
                   std::optional<blink::UserAgentOverride> ua_override,
                   std::string fingerprint_json,
                   blink::mojom::WebRtcIpHandlingPolicy webrtc_policy);
  MontiUaTabHelper(const MontiUaTabHelper&) = delete;
  MontiUaTabHelper& operator=(const MontiUaTabHelper&) = delete;
  ~MontiUaTabHelper() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;

 private:
  // Applies the identity to `web_contents` and watches its navigations.
  class TabObserver;

  void StartObserving(content::WebContents* web_contents);
  void StopObserving(content::WebContents* web_contents);

  const std::optional<blink::UserAgentOverride> ua_override_;
  const std::string fingerprint_json_;
  const blink::mojom::WebRtcIpHandlingPolicy webrtc_policy_;
  raw_ptr<TabStripModel> tab_strip_model_;
  std::map<content::WebContents*, std::unique_ptr<TabObserver>> tab_observers_;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_UA_TAB_HELPER_H_

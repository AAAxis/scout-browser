// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_H_
#define CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_H_

#include <memory>
#include <optional>
#include <string>

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/values.h"
#include "chrome/browser/monti/monti_profile_draft.h"
#include "chrome/browser/monti/monti_proxy.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;
class PrefService;

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace os_crypt_async {
class Encryptor;
}  // namespace os_crypt_async

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace monti {

// Per-profile Monti metadata, persisted in the profile's own prefs (and thus
// isolated per profile and surviving restart for free). Holds the display
// name, the assigned proxy id, the saved SOCKS5 endpoint, and an opaque
// `fingerprint` dict reserved for native spoofing in optional session 07.
//
// This service is *also* the single source of truth for the profile's proxy
// connection: it owns the SOCKS5 bridge lifecycle and the `kProxy` pref so the
// state is correct whether or not the side panel is open. Crucially it runs a
// fail-safe at startup (forces a direct connection before anything else) so a
// stale proxy pref from a previous session can never black-hole all traffic.
class MontiProfileService : public KeyedService {
 public:
  enum class ConnectionState {
    kDirect,      // no proxy; traffic goes out directly
    kConnecting,  // bridge starting / egress being verified
    kConnected,   // verified, traffic egresses through the proxy
  };

  // Observers are notified whenever the connection state (or the active proxy)
  // changes. Used by the omnibox Monti status chip.
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnMontiConnectionStateChanged(MontiProfileService* service) {}
  };

  // Reports the outcome of a Connect() attempt. `error` is empty on success.
  using ConnectCallback =
      base::OnceCallback<void(bool success, const std::string& error)>;

  // Reports the outcome of a ProbeProxy() test. `egress_ip` is the verified
  // public exit IP on success (empty on failure); `country` is the geo-resolved
  // ISO country code of that egress IP (empty if unavailable); `error` is empty
  // on success.
  using ProbeCallback = base::OnceCallback<void(
      bool ok, const std::string& egress_ip, const std::string& country,
      const std::string& error)>;

  explicit MontiProfileService(Profile* profile);

  MontiProfileService(const MontiProfileService&) = delete;
  MontiProfileService& operator=(const MontiProfileService&) = delete;

  ~MontiProfileService() override;

  // Registers the dict pref backing this service.
  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  std::string name() const;
  void SetName(const std::string& name);

  std::string assigned_proxy_id() const;
  void SetAssignedProxyId(const std::string& proxy_id);

  // The SOCKS5 endpoint last used for this profile (empty `host` if none). Kept
  // for form repopulation and auto-reconnect; independent of whether we are
  // currently connected.
  MontiProxy GetSavedProxy() const;
  void SetSavedProxy(const MontiProxy& proxy);

  // Returns a copy of the (currently always empty) fingerprint dict.
  base::DictValue fingerprint() const;
  void SetFingerprint(base::DictValue fingerprint);

  // Reports the decrypted credentials list.
  using CredentialsCallback =
      base::OnceCallback<void(std::vector<ProfileCredential>)>;

  // Per-platform login credentials, persisted OSCrypt-encrypted (never
  // plaintext). Set is fire-and-forget (encrypts once the process encryptor is
  // ready, then writes prefs); Get decrypts asynchronously. The login-page
  // auto-fill that consumes these is deferred to a later session.
  void SetCredentials(std::vector<ProfileCredential> credentials);
  void GetCredentials(CredentialsCallback callback);

  // The New-profile dialog's cookie draft blob (pasted/uploaded text). Held only;
  // injection into the profile's cookie store is deferred to a later session.
  std::string cookies() const;
  void SetCookies(const std::string& cookies);

  // Additive bookkeeping written by the rich create flow for later sessions to
  // read (status chip, profile-type defaults, seeded start pages). Not enforced
  // here.
  std::string status() const;
  void SetStatus(const std::string& status);
  std::string profile_type() const;
  void SetProfileType(const std::string& profile_type);
  std::vector<std::string> start_pages() const;
  void SetStartPages(const std::vector<std::string>& start_pages);

  // Authoritative connection state.
  ConnectionState connection_state() const { return state_; }
  const std::string& connected_host() const { return active_proxy_.host; }
  int connected_port() const { return active_proxy_.socks_port; }
  // Id of the active proxy (empty when direct).
  const std::string& connected_proxy_id() const { return active_proxy_.id; }
  // Country of the active proxy (empty when direct or unknown).
  const std::string& connected_country() const { return active_proxy_.country; }
  // Verified public egress IP of the active proxy (empty unless connected).
  const std::string& connected_egress_ip() const { return last_egress_ip_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Connects the profile through `proxy`: starts the local SOCKS5 bridge,
  // points the profile at it, and verifies egress before reporting success.
  // On any failure it reverts the profile to a direct connection (never leaves
  // traffic black-holed) and reports the error.
  void Connect(const MontiProxy& proxy, ConnectCallback callback);

  // Tests `proxy` without persisting anything: applies it to the profile,
  // verifies real egress (a public IP that differs from the direct/baseline
  // IP), then reverts to a direct connection. Used by the Proxies-tab Test
  // button so a test never pollutes the saved/assigned state or the proxy list.
  void ProbeProxy(const MontiProxy& proxy, ProbeCallback callback);

  // Reverts the profile to a direct connection and forgets the
  // "should be connected" intent so the next startup does not auto-reconnect.
  void Disconnect(base::OnceClosure callback);

  // Applies `proxy` via the local SocksBridge (auth handshake upstream,
  // unauthenticated SOCKS5 presented to Chromium) with none of Connect()'s
  // egress verification, fail-closed gating, or revert-to-direct-on-failure
  // behavior. Monti Anty verifies the assigned proxy is reachable before ever
  // spawning this process, so re-checking it here (and silently reverting to
  // a direct, unproxied connection if that check flakes) only adds a
  // fail-closed page nobody could act on plus a real risk of a silent IP leak
  // once nothing surfaces that revert to the user. Used for the initial
  // command-line-driven launch; the interactive "Connect" flow (side panel)
  // still uses Connect() above, which does verify and reports back to that UI.
  void ApplyAssignedProxy(const MontiProxy& proxy);

 private:
  // Forces the profile to a direct connection (fail-safe) and then, if a proxy
  // was assigned last session, attempts a verified auto-reconnect. Posted from
  // the constructor so the network stack is ready when it runs.
  void InitializeAsync();

  // Shared two-phase verification core. Captures a baseline (pre-proxy) egress
  // IP, applies `proxy` (HTTP pref or SOCKS5 bridge), then confirms traffic
  // actually egresses through it: a valid, non-loopback public IP that differs
  // from the baseline. `persist` keeps the proxy applied and marks kConnected on
  // success; otherwise the profile is reverted to direct before reporting.
  using VerifyCallback = base::OnceCallback<void(
      bool ok, const std::string& egress_ip, const std::string& country,
      const std::string& error)>;
  void StartVerification(MontiProxy proxy,
                         bool persist,
                         VerifyCallback callback);
  void OnBaselineFetched(MontiProxy proxy,
                         bool persist,
                         VerifyCallback callback,
                         std::optional<std::string> body);
  void OnVerifyBridgeStarted(MontiProxy proxy,
                             bool persist,
                             std::string baseline_ip,
                             VerifyCallback callback,
                             int local_port);
  void StartProxiedFetch(MontiProxy proxy,
                         bool persist,
                         std::string baseline_ip,
                         int attempt,
                         VerifyCallback callback);
  void OnProxiedFetched(MontiProxy proxy,
                        bool persist,
                        std::string baseline_ip,
                        int attempt,
                        VerifyCallback callback,
                        std::optional<std::string> body);
  void FinishVerification(MontiProxy proxy,
                          bool persist,
                          const std::string& egress_ip,
                          const std::string& country,
                          bool ok,
                          const std::string& error,
                          VerifyCallback callback);

  // Bridge-start reply for ApplyAssignedProxy(). A local_port <= 0 means the
  // bridge itself failed to bind a loopback listener (a local environment
  // problem, not a proxy-reachability one) -- logged, not gated.
  void OnAssignedProxyBridgeStarted(MontiProxy proxy, int local_port);

  // Adapts the shared VerifyCallback back to Connect()'s 2-arg callback.
  void OnConnectVerified(ConnectCallback callback,
                         bool ok,
                         const std::string& egress_ip,
                         const std::string& country,
                         const std::string& error);

  // Fires a single egress probe through whatever proxy config is currently
  // applied to the profile. `disable_cache` bypasses the HTTP cache so a
  // second probe is never served the first probe's cached body.
  void StartEgressLoader(
      bool disable_cache,
      base::OnceCallback<void(std::optional<std::string>)> on_done);

  // Reverts `kProxy` to direct and clears the runtime active-proxy state.
  void RevertToDirect();
  void SetState(ConnectionState state);

  // OSCrypt encrypt/decrypt continuations, run once the process-wide async
  // encryptor instance is available.
  void EncryptAndStoreCredentials(
      std::vector<ProfileCredential> credentials,
      scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void DecryptCredentials(CredentialsCallback callback,
                          scoped_refptr<os_crypt_async::Encryptor> encryptor);

  const raw_ptr<Profile> profile_;
  const raw_ptr<PrefService> prefs_;

  ConnectionState state_ = ConnectionState::kDirect;
  MontiProxy active_proxy_;
  std::string last_egress_ip_;
  int bridge_port_ = 0;
  std::unique_ptr<network::SimpleURLLoader> egress_loader_;

  // Callbacks that arrived while a Connect() verification was already in flight
  // for the same proxy. All fired (with the same result) when verification ends.
  std::vector<ConnectCallback> pending_connect_callbacks_;

  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<MontiProfileService> weak_factory_{this};
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_H_

// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_profile_service.h"

#include <utility>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/monti/monti_pref_names.h"
#include "chrome/browser/monti/proxy_applicator.h"
#include "chrome/browser/monti/socks_bridge.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_utils.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/ip_address.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "url/gurl.h"

namespace monti {

namespace {

// Single dict pref that holds all Monti per-profile metadata.
constexpr char kMontiProfileData[] = "monti.profile_data";

constexpr char kNameKey[] = "name";
constexpr char kAssignedProxyIdKey[] = "assigned_proxy_id";
constexpr char kFingerprintKey[] = "fingerprint";

// Rich-create (session 20) sub-keys. `credentials` holds a list of dicts whose
// email/password are OSCrypt-encrypted then base64-encoded (never plaintext).
constexpr char kCredentialsKey[] = "credentials";
constexpr char kCredentialPlatformKey[] = "platform";
constexpr char kCredentialEmailKey[] = "email";
constexpr char kCredentialPasswordKey[] = "password";
constexpr char kCookiesKey[] = "cookies_draft";
constexpr char kStatusKey[] = "status";
constexpr char kProfileTypeKey[] = "profile_type";
constexpr char kStartPagesKey[] = "start_pages";

// Saved-endpoint sub-keys (so a connected proxy survives restart for the form
// and for verified auto-reconnect, without depending on the async ProxyStore).
constexpr char kProxyHostKey[] = "proxy_host";
constexpr char kProxyPortKey[] = "proxy_port";
constexpr char kProxyHttpPortKey[] = "proxy_http_port";
constexpr char kProxyUsernameKey[] = "proxy_username";
constexpr char kProxyPasswordKey[] = "proxy_password";
constexpr char kProxyTransportKey[] = "proxy_transport";

// Egress probe: a tiny request that only succeeds if traffic actually reaches
// the internet through the freshly-applied proxy. The endpoint returns JSON
// with both the public IP and the geo-resolved country code, so a single probe
// both verifies egress and discovers the proxy's real exit country.
constexpr char kEgressCheckUrl[] = "https://ipwho.is/";
constexpr base::TimeDelta kEgressCheckTimeout = base::Seconds(8);
constexpr int kEgressCheckMaxBytes = 8192;

// The proxy pref (and, for authenticated HTTP proxies, the seeded credentials)
// propagate to the NetworkContext asynchronously, so the first proxied probe
// can race: it may egress directly, or 407 before the auth cache lands. We
// retry a few short times to let the config settle before declaring failure.
// A genuinely dead proxy simply fails every attempt (each bounded by the probe
// timeout) and is then reported unreachable.
constexpr int kProxiedProbeMaxAttempts = 5;
constexpr base::TimeDelta kProxiedProbeRetryDelay = base::Milliseconds(300);
// kEgressCheckUrl is a single shared free-tier endpoint used by every profile
// launch (the baseline call always comes from this machine's one real IP, and
// many proxy providers' exit IPs are themselves rate-limited or deprioritized
// by IP-intelligence services). Launching several profiles back-to-back can
// trip its rate limit well before any real proxy problem, and retrying at
// kProxiedProbeRetryDelay just re-triggers the same limit. Back off much
// longer specifically on a 429 so the retry has a real chance of landing
// outside the rate-limit window instead of burning the whole attempt budget
// on a proxy that was never actually broken.
constexpr base::TimeDelta kProxiedProbeRateLimitDelay = base::Seconds(3);

// The validated public egress IP and (best-effort) ISO country code parsed from
// an egress-probe response.
struct EgressInfo {
  std::string ip;       // empty unless a public, non-loopback IP was returned
  std::string country;  // lowercased ISO-3166 alpha-2, empty if unavailable
};

// Parses the egress-probe body. Accepts the JSON geo endpoint
// ({"ip":"...","country_code":"US",...}) and tolerates the legacy plain-text
// IP-echo shape as a fallback. `ip` is only populated for a public,
// non-loopback address (otherwise the request never really left the host).
EgressInfo ParseEgress(const std::optional<std::string>& body) {
  EgressInfo info;
  if (!body.has_value()) {
    return info;
  }
  const std::string trimmed(base::TrimWhitespaceASCII(*body, base::TRIM_ALL));

  // JSON first; on failure fall back to treating the whole body as a bare IP.
  std::string ip_candidate = trimmed;
  if (std::optional<base::DictValue> dict =
          base::JSONReader::ReadDict(trimmed, base::JSON_PARSE_RFC)) {
    const std::string* ip = dict->FindString("ip");
    if (!ip) {
      ip = dict->FindString("query");  // ip-api.com shape
    }
    if (ip) {
      ip_candidate = *ip;
    }
    const std::string* cc = dict->FindString("country_code");
    if (!cc) {
      cc = dict->FindString("countryCode");  // ip-api.com shape
    }
    if (cc) {
      info.country = base::ToLowerASCII(*cc);
    }
  }

  net::IPAddress address;
  if (address.AssignFromIPLiteral(ip_candidate) && !address.IsLoopback() &&
      address.IsPubliclyRoutable()) {
    info.ip = ip_candidate;
  }
  return info;
}

}  // namespace

MontiProfileService::MontiProfileService(Profile* profile)
    : profile_(profile), prefs_(profile->GetPrefs()) {
  // Privacy fail-closed defaults. Sites should never receive host geolocation
  // from the OS, and WebRTC must not expose non-proxied/local interfaces while
  // the profile is starting or waiting for proxy verification.
  prefs_->SetString(::prefs::kWebRTCIPHandlingPolicy,
                    blink::kWebRTCIPHandlingDisableNonProxiedUdp);
  if (HostContentSettingsMap* settings =
          HostContentSettingsMapFactory::GetForProfile(profile_)) {
    const ContentSettingsType geolocation_type =
        content_settings::GeolocationContentSettingsType();
    if (geolocation_type == ContentSettingsType::GEOLOCATION_WITH_OPTIONS) {
      settings->SetDefaultPermissionSetting(
          geolocation_type,
          GeolocationSetting{PermissionOption::kDenied,
                             PermissionOption::kDenied});
    } else {
      settings->SetDefaultContentSetting(geolocation_type,
                                         CONTENT_SETTING_BLOCK);
    }
  }

  // Fail-safe: before anything navigates, force a direct connection so a
  // `socks5://127.0.0.1:<old_port>` left in prefs from a previous session (whose
  // bridge is gone) can never black-hole all traffic. The verified
  // auto-reconnect (if a proxy was assigned) runs asynchronously below, once the
  // network stack is ready.
  //
  // --monti-profile-launch sessions skip this: StartupBrowserCreatorImpl's
  // Monti-launch flow applies and verifies the assigned proxy synchronously
  // before any navigation and never falls back to direct on failure (it opens
  // a local error page instead), so clearing to direct here would only
  // introduce a window where the profile is briefly configured for direct
  // traffic instead of staying fail-closed.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          "monti-profile-launch")) {
    ClearProxyFromProfile(profile_);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MontiProfileService::InitializeAsync,
                                weak_factory_.GetWeakPtr()));
}

MontiProfileService::~MontiProfileService() = default;

// static
void MontiProfileService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kMontiProfileData);
  // chrome://monti-newtab toggle (default on) + its per-profile favorites grid.
  registry->RegisterBooleanPref(monti::prefs::kMontiUseMontiNewTab, true);
  registry->RegisterListPref(monti::prefs::kMontiFavorites);

  // chrome://monti-settings (session 34): anti-detect defaults, appearance,
  // and notification toggles.
  registry->RegisterStringPref(monti::prefs::kMontiDefaultFingerprintPreset,
                               std::string());
  registry->RegisterDictionaryPref(monti::prefs::kMontiDefaultSpoofModes);
  registry->RegisterBooleanPref(monti::prefs::kMontiTimezoneFollowsProxy,
                                false);
  registry->RegisterBooleanPref(monti::prefs::kMontiWebrtcLeakProtection, true);
  registry->RegisterStringPref(monti::prefs::kMontiTheme, "system");
  registry->RegisterStringPref(monti::prefs::kMontiDefaultSearchEngine,
                               std::string());
  registry->RegisterBooleanPref(monti::prefs::kMontiNotifyBans, true);
  registry->RegisterBooleanPref(monti::prefs::kMontiNotifyProxyDown, true);
  registry->RegisterBooleanPref(monti::prefs::kMontiNotifySync, true);
}

std::string MontiProfileService::name() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const std::string* value = dict.FindString(kNameKey);
  return value ? *value : std::string();
}

void MontiProfileService::SetName(const std::string& name) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kNameKey, name);
}

std::string MontiProfileService::assigned_proxy_id() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const std::string* value = dict.FindString(kAssignedProxyIdKey);
  return value ? *value : std::string();
}

void MontiProfileService::SetAssignedProxyId(const std::string& proxy_id) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kAssignedProxyIdKey, proxy_id);
}

MontiProxy MontiProfileService::GetSavedProxy() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  MontiProxy proxy;
  if (const std::string* host = dict.FindString(kProxyHostKey)) {
    proxy.host = *host;
  }
  proxy.socks_port = dict.FindInt(kProxyPortKey).value_or(0);
  proxy.http_port = dict.FindInt(kProxyHttpPortKey).value_or(0);
  if (const std::string* user = dict.FindString(kProxyUsernameKey)) {
    proxy.username = *user;
  }
  if (const std::string* pass = dict.FindString(kProxyPasswordKey)) {
    proxy.password = *pass;
  }
  proxy.id = assigned_proxy_id();
  // Default to socks5 for back-compat with endpoints saved before HTTP proxies
  // were supported.
  if (const std::string* transport = dict.FindString(kProxyTransportKey)) {
    proxy.transport = *transport;
  } else {
    proxy.transport = "socks5";
  }
  return proxy;
}

void MontiProfileService::SetSavedProxy(const MontiProxy& proxy) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kProxyHostKey, proxy.host);
  update->Set(kProxyPortKey, proxy.socks_port);
  update->Set(kProxyHttpPortKey, proxy.http_port);
  update->Set(kProxyUsernameKey, proxy.username);
  update->Set(kProxyPasswordKey, proxy.password);
  update->Set(kProxyTransportKey, proxy.transport);
}

base::DictValue MontiProfileService::fingerprint() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const base::DictValue* value = dict.FindDict(kFingerprintKey);
  return value ? value->Clone() : base::DictValue();
}

void MontiProfileService::SetFingerprint(base::DictValue fingerprint) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kFingerprintKey, std::move(fingerprint));
}

void MontiProfileService::SetCredentials(
    std::vector<ProfileCredential> credentials) {
  // The process-wide async encryptor is ready long before a user creates a
  // profile, so this callback typically runs synchronously. Either way we never
  // write plaintext: the prefs are only touched inside the continuation.
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &MontiProfileService::EncryptAndStoreCredentials,
      weak_factory_.GetWeakPtr(), std::move(credentials)));
}

void MontiProfileService::EncryptAndStoreCredentials(
    std::vector<ProfileCredential> credentials,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  base::ListValue list;
  for (const ProfileCredential& cred : credentials) {
    std::string email_ciphertext;
    std::string password_ciphertext;
    if (!encryptor->EncryptString(cred.email, &email_ciphertext) ||
        !encryptor->EncryptString(cred.password, &password_ciphertext)) {
      // Encryption unavailable: skip rather than persist anything readable.
      continue;
    }
    base::DictValue entry;
    entry.Set(kCredentialPlatformKey, cred.platform);
    entry.Set(kCredentialEmailKey, base::Base64Encode(email_ciphertext));
    entry.Set(kCredentialPasswordKey, base::Base64Encode(password_ciphertext));
    list.Append(std::move(entry));
  }
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kCredentialsKey, std::move(list));
}

void MontiProfileService::GetCredentials(CredentialsCallback callback) {
  g_browser_process->os_crypt_async()->GetInstance(
      base::BindOnce(&MontiProfileService::DecryptCredentials,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void MontiProfileService::DecryptCredentials(
    CredentialsCallback callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::vector<ProfileCredential> out;
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  if (const base::ListValue* list = dict.FindList(kCredentialsKey)) {
    for (const base::Value& value : *list) {
      const base::DictValue* entry = value.GetIfDict();
      if (!entry) {
        continue;
      }
      ProfileCredential cred;
      if (const std::string* platform =
              entry->FindString(kCredentialPlatformKey)) {
        cred.platform = *platform;
      }
      auto decrypt = [&encryptor](const std::string* b64, std::string& out) {
        std::string ciphertext;
        std::string plaintext;
        if (b64 && base::Base64Decode(*b64, &ciphertext) &&
            encryptor->DecryptString(ciphertext, &plaintext)) {
          out = plaintext;
        }
      };
      decrypt(entry->FindString(kCredentialEmailKey), cred.email);
      decrypt(entry->FindString(kCredentialPasswordKey), cred.password);
      out.push_back(std::move(cred));
    }
  }
  std::move(callback).Run(std::move(out));
}

std::string MontiProfileService::cookies() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const std::string* value = dict.FindString(kCookiesKey);
  return value ? *value : std::string();
}

void MontiProfileService::SetCookies(const std::string& cookies) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kCookiesKey, cookies);
}

std::string MontiProfileService::status() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const std::string* value = dict.FindString(kStatusKey);
  return value ? *value : std::string();
}

void MontiProfileService::SetStatus(const std::string& status) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kStatusKey, status);
}

std::string MontiProfileService::profile_type() const {
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  const std::string* value = dict.FindString(kProfileTypeKey);
  return value ? *value : std::string();
}

void MontiProfileService::SetProfileType(const std::string& profile_type) {
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kProfileTypeKey, profile_type);
}

std::vector<std::string> MontiProfileService::start_pages() const {
  std::vector<std::string> out;
  const base::DictValue& dict = prefs_->GetDict(kMontiProfileData);
  if (const base::ListValue* list = dict.FindList(kStartPagesKey)) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        out.push_back(value.GetString());
      }
    }
  }
  return out;
}

void MontiProfileService::SetStartPages(
    const std::vector<std::string>& start_pages) {
  base::ListValue list;
  for (const std::string& page : start_pages) {
    list.Append(page);
  }
  ScopedDictPrefUpdate update(prefs_, kMontiProfileData);
  update->Set(kStartPagesKey, std::move(list));
}

void MontiProfileService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void MontiProfileService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void MontiProfileService::Connect(const MontiProxy& proxy,
                                  ConnectCallback callback) {
  // Doppler endpoints are SOCKS5; list proxies imported from CSV are HTTP.
  const bool is_socks = proxy.transport == "socks5";
  const bool valid = !proxy.host.empty() &&
                     (is_socks ? proxy.socks_port > 0 : proxy.http_port > 0);
  if (!valid) {
    std::move(callback).Run(false, "Invalid configuration");
    return;
  }

  // A verification for this exact proxy is already in flight (e.g. triggered
  // by InitializeAsync before MontiManager's launch callback arrived). Queue
  // this callback — it will fire with the same result when verification ends,
  // avoiding a second concurrent verify that would cancel the first's loaders.
  if (state_ == ConnectionState::kConnecting && active_proxy_.id == proxy.id) {
    pending_connect_callbacks_.push_back(std::move(callback));
    return;
  }

  // Already verified and connected: report success immediately so the caller
  // can open the window without re-running the egress check.
  if (state_ == ConnectionState::kConnected && active_proxy_.id == proxy.id) {
    std::move(callback).Run(true, std::string());
    return;
  }

  // Starting a fresh connection: drop stale pending callbacks from any previous
  // aborted connection and begin a new verification cycle.
  pending_connect_callbacks_.clear();

  // Reflect the in-flight target in the omnibox status chip immediately.
  active_proxy_ = proxy;
  SetState(ConnectionState::kConnecting);

  // Persist the intent and the endpoint so we can repopulate the form and
  // auto-reconnect next session. The proxy is intentionally NOT added to the
  // shared ProxyStore: a side-panel connection must never pollute the
  // user-managed Proxies-tab list.
  SetAssignedProxyId(proxy.id);
  SetSavedProxy(proxy);
  prefs_->SetString(::prefs::kWebRTCIPHandlingPolicy,
                    blink::kWebRTCIPHandlingDisableNonProxiedUdp);

  StartVerification(proxy, /*persist=*/true,
                    base::BindOnce(&MontiProfileService::OnConnectVerified,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(callback)));
}

void MontiProfileService::OnConnectVerified(ConnectCallback callback,
                                            bool ok,
                                            const std::string& egress_ip,
                                            const std::string& country,
                                            const std::string& error) {
  std::move(callback).Run(ok, error);
  // Drain any callbacks that were queued while this verification was in flight.
  auto pending = std::move(pending_connect_callbacks_);
  for (ConnectCallback& cb : pending) {
    std::move(cb).Run(ok, error);
  }
}

void MontiProfileService::ProbeProxy(const MontiProxy& proxy,
                                     ProbeCallback callback) {
  const bool is_socks = proxy.transport == "socks5";
  const bool valid = !proxy.host.empty() &&
                     (is_socks ? proxy.socks_port > 0 : proxy.http_port > 0);
  if (!valid) {
    std::move(callback).Run(false, std::string(), std::string(),
                            "Invalid configuration");
    return;
  }
  // A pure test: never touch assigned/saved state or the ProxyStore. The
  // verification core applies the proxy, confirms real egress, then reverts to
  // a direct connection. ProbeCallback and VerifyCallback share a signature, so
  // the caller's callback forwards straight through.
  StartVerification(proxy, /*persist=*/false, std::move(callback));
}

void MontiProfileService::Disconnect(base::OnceClosure callback) {
  const std::string id = assigned_proxy_id();
  if (!id.empty()) {
    SocksBridge::GetInstance()->StopBridge(id);
  }
  // User explicitly disconnected: forget the intent so startup stays direct.
  SetAssignedProxyId(std::string());
  egress_loader_.reset();
  RevertToDirect();
  if (callback) {
    std::move(callback).Run();
  }
}

void MontiProfileService::ApplyAssignedProxy(const MontiProxy& proxy) {
  active_proxy_ = proxy;
  SetAssignedProxyId(proxy.id);
  SetSavedProxy(proxy);
  prefs_->SetString(::prefs::kWebRTCIPHandlingPolicy,
                    blink::kWebRTCIPHandlingDisableNonProxiedUdp);
  SetState(ConnectionState::kConnecting);
  SocksBridge::GetInstance()->StartBridge(
      proxy, base::BindOnce(&MontiProfileService::OnAssignedProxyBridgeStarted,
                            weak_factory_.GetWeakPtr(), proxy));
}

void MontiProfileService::OnAssignedProxyBridgeStarted(MontiProxy proxy,
                                                        int local_port) {
  if (local_port <= 0) {
    LOG(ERROR) << "Monti: local proxy bridge failed to start for "
              << proxy.host << " -- profile stays on whatever proxy config "
              << "it already had (no revert-to-direct here; see "
              << "ApplyAssignedProxy).";
    SetState(ConnectionState::kDirect);
    return;
  }
  bridge_port_ = local_port;
  ApplySocksProxyToProfile(profile_, local_port);
  SetState(ConnectionState::kConnected);
}

void MontiProfileService::InitializeAsync() {
  // Only auto-reconnect if a proxy was connected last session and we have not
  // since changed state (e.g. the user already interacted with the panel).
  if (state_ != ConnectionState::kDirect || assigned_proxy_id().empty()) {
    return;
  }
  MontiProxy proxy = GetSavedProxy();
  const bool is_socks = proxy.transport == "socks5";
  const bool valid = !proxy.host.empty() &&
                     (is_socks ? proxy.socks_port > 0 : proxy.http_port > 0);
  if (!valid) {
    return;
  }
  Connect(proxy, base::DoNothing());
}

void MontiProfileService::StartVerification(MontiProxy proxy,
                                            bool persist,
                                            VerifyCallback callback) {
  // Phase 1: capture the baseline egress IP under the *current* (pre-proxy)
  // config. Comparing the proxied IP against this baseline is what catches a
  // silent direct fallback when the proxy pref hasn't propagated yet.
  StartEgressLoader(
      /*disable_cache=*/true,
      base::BindOnce(&MontiProfileService::OnBaselineFetched,
                     weak_factory_.GetWeakPtr(), std::move(proxy), persist,
                     std::move(callback)));
}

void MontiProfileService::OnBaselineFetched(MontiProxy proxy,
                                            bool persist,
                                            VerifyCallback callback,
                                            std::optional<std::string> body) {
  egress_loader_.reset();
  std::string baseline_ip = ParseEgress(body).ip;

  // Phase 2: stand up the local bridge, then apply it. For SOCKS5 upstreams it
  // performs RFC 1929 auth; for HTTP upstreams it performs an authenticated
  // CONNECT. Either way Chromium only sees an unauthenticated localhost SOCKS5
  // proxy, avoiding proxy-auth 407 dialogs and auth-cache realm mismatches.
  SocksBridge::GetInstance()->StartBridge(
      proxy, base::BindOnce(&MontiProfileService::OnVerifyBridgeStarted,
                            weak_factory_.GetWeakPtr(), proxy, persist,
                            std::move(baseline_ip), std::move(callback)));
}

void MontiProfileService::OnVerifyBridgeStarted(MontiProxy proxy,
                                                bool persist,
                                                std::string baseline_ip,
                                                VerifyCallback callback,
                                                int local_port) {
  if (local_port <= 0) {
    FinishVerification(std::move(proxy), persist, std::string(),
                       /*country=*/std::string(), /*ok=*/false,
                       "Failed to start proxy bridge", std::move(callback));
    return;
  }
  bridge_port_ = local_port;
  ApplySocksProxyToProfile(profile_, local_port);
  StartProxiedFetch(std::move(proxy), persist, std::move(baseline_ip),
                    /*attempt=*/0, std::move(callback));
}

void MontiProfileService::StartProxiedFetch(MontiProxy proxy,
                                            bool persist,
                                            std::string baseline_ip,
                                            int attempt,
                                            VerifyCallback callback) {
  StartEgressLoader(
      /*disable_cache=*/true,
      base::BindOnce(&MontiProfileService::OnProxiedFetched,
                     weak_factory_.GetWeakPtr(), std::move(proxy), persist,
                     std::move(baseline_ip), attempt, std::move(callback)));
}

void MontiProfileService::OnProxiedFetched(MontiProxy proxy,
                                           bool persist,
                                           std::string baseline_ip,
                                           int attempt,
                                           VerifyCallback callback,
                                           std::optional<std::string> body) {
  // Read transport-level status BEFORE releasing the loader.
  const int net_error =
      egress_loader_ ? egress_loader_->NetError() : net::ERR_FAILED;
  int response_code = 0;
  if (egress_loader_ && egress_loader_->ResponseInfo() &&
      egress_loader_->ResponseInfo()->headers) {
    response_code = egress_loader_->ResponseInfo()->headers->response_code();
  }
  egress_loader_.reset();

  const EgressInfo info = ParseEgress(body);
  const std::string& egress_ip = info.ip;
  const bool probe_ok = net_error == net::OK && response_code >= 200 &&
                        response_code <= 299 && !egress_ip.empty();
  // Verified only if a valid public egress IP came back AND it differs from the
  // direct baseline (i.e. traffic genuinely went through the proxy, not a
  // silent direct fallback).
  const bool verified = probe_ok && egress_ip != baseline_ip;

  if (verified) {
    FinishVerification(std::move(proxy), persist, egress_ip, info.country,
                       /*ok=*/true, std::string(), std::move(callback));
    return;
  }

  if (attempt + 1 < kProxiedProbeMaxAttempts) {
    // Not yet verified: the proxy config / auth cache may not have reached the
    // NetworkContext, or the request raced onto a direct connection. Wait
    // briefly and retry before declaring the proxy unreachable. A 429 means
    // the egress-check endpoint itself is rate-limiting this machine, not
    // that the proxy is broken, so give that case a much longer runway.
    const base::TimeDelta delay = response_code == 429
                                      ? kProxiedProbeRateLimitDelay
                                      : kProxiedProbeRetryDelay;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&MontiProfileService::StartProxiedFetch,
                       weak_factory_.GetWeakPtr(), std::move(proxy), persist,
                       std::move(baseline_ip), attempt + 1,
                       std::move(callback)),
        delay);
    return;
  }

  // Exhausted retries. Report the specific reason so the UI can distinguish a
  // proxy-auth rejection (407) from an unreachable endpoint or a silent direct
  // fallback, instead of a blanket "unreachable".
  std::string detail;
  if (response_code == 429) {
    detail = "Proxy verification rate-limited (HTTP 429) — the egress-check "
             "service is throttling this machine, not necessarily the proxy; "
             "wait a moment and try again";
  } else if (response_code == 407) {
    detail = "Proxy authentication failed (HTTP 407)";
  } else if (net_error != net::OK) {
    detail = base::StrCat(
        {"Proxy unreachable (", net::ErrorToShortString(net_error), ")"});
  } else if (response_code != 0 && (response_code < 200 || response_code > 299)) {
    detail = base::StrCat(
        {"Proxy returned HTTP ", base::NumberToString(response_code)});
  } else if (egress_ip.empty()) {
    detail = "Proxy returned no valid public IP";
  } else {
    detail = "Traffic did not route through the proxy";
  }
  FinishVerification(std::move(proxy), persist, std::string(),
                     /*country=*/std::string(), /*ok=*/false, detail,
                     std::move(callback));
}

void MontiProfileService::FinishVerification(MontiProxy proxy,
                                             bool persist,
                                             const std::string& egress_ip,
                                             const std::string& country,
                                             bool ok,
                                             const std::string& error,
                                             VerifyCallback callback) {
  if (ok && persist) {
    // Keep the proxy applied and mark the profile connected. Adopt the
    // geo-resolved egress country when available (more authoritative than the
    // CSV-declared value).
    active_proxy_ = std::move(proxy);
    if (!country.empty()) {
      active_proxy_.country = country;
    }
    last_egress_ip_ = egress_ip;
    SetState(ConnectionState::kConnected);
    std::move(callback).Run(true, egress_ip, active_proxy_.country,
                            std::string());
    return;
  }

  // Test-only success, or any failure: tear down the (possibly running) bridge
  // and revert to a direct connection so the profile keeps working internet.
  SocksBridge::GetInstance()->StopBridge(proxy.id);
  RevertToDirect();
  std::move(callback).Run(ok, ok ? egress_ip : std::string(),
                          ok ? country : std::string(), error);
}

void MontiProfileService::StartEgressLoader(
    bool disable_cache,
    base::OnceCallback<void(std::optional<std::string>)> on_done) {
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("monti_proxy_egress_check", R"(
        semantics {
          sender: "Monti Proxy"
          description:
            "After applying a user-configured proxy, Monti fetches a small "
            "IP-echo endpoint to confirm traffic actually egresses through the "
            "proxy (a public IP that differs from the direct connection) before "
            "reporting the proxy as working. If the probe fails the profile is "
            "reverted to a direct connection so the user never loses internet "
            "access."
          trigger:
            "The user connects to or tests a proxy in Monti, or Monti "
            "auto-reconnects to the last proxy at startup."
          data: "None. Only an HTTP GET to a public IP-echo endpoint."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "Triggered only by an explicit proxy connection or test in "
            "Monti."
          policy_exception_justification: "Not implemented."
        })");

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(kEgressCheckUrl);
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  if (disable_cache) {
    // Without this the proxied probe can be served the baseline probe's cached
    // body, defeating the IP comparison.
    request->load_flags |= net::LOAD_DISABLE_CACHE;
  }

  egress_loader_ =
      network::SimpleURLLoader::Create(std::move(request), traffic_annotation);
  egress_loader_->SetTimeoutDuration(kEgressCheckTimeout);
  egress_loader_->DownloadToString(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      std::move(on_done), kEgressCheckMaxBytes);
}

void MontiProfileService::RevertToDirect() {
  ClearProxyFromProfile(profile_);
  bridge_port_ = 0;
  active_proxy_ = MontiProxy();
  last_egress_ip_.clear();
  SetState(ConnectionState::kDirect);
}

void MontiProfileService::SetState(ConnectionState state) {
  state_ = state;
  for (Observer& observer : observers_) {
    observer.OnMontiConnectionStateChanged(this);
  }
}

}  // namespace monti

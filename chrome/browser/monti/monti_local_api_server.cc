// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_local_api_server.h"

#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/monti/monti_fingerprint.h"
#include "chrome/browser/monti/monti_profile_draft.h"
#include "chrome/browser/monti/monti_profile_service.h"
#include "chrome/browser/monti/monti_profile_service_factory.h"
#include "chrome/browser/monti/profile_store.h"
#include "chrome/browser/monti/proxy_store.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/http/http_status_code.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "url/gurl.h"

namespace monti {

namespace {

constexpr int kBacklog = 10;

constexpr net::NetworkTrafficAnnotationTag kLocalApiAnnotation =
    net::DefineNetworkTrafficAnnotation("monti_local_api_server", R"(
      semantics {
        sender: "Monti Local API Server"
        description:
          "Monti exposes a local REST API on 127.0.0.1:39217 so that external
           automation scripts can control profile, proxy, and cookie management
           without UI interaction."
        trigger:
          "An HTTP request arrives from a local client (e.g. a Node.js
           automation script) on the loopback interface."
        data:
          "JSON-serialised Monti profile metadata, proxy records, and browser
           cookie data.  All traffic stays on the local machine."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting:
          "The local API server is always active while Monti is running."
        policy_exception_justification:
          "No enterprise policy controls this purely local server."
      })");

// ---- URL helpers ------------------------------------------------------------

// Local interstitial page shown when the Monti binary is started without
// going through Monti Anty at all. Proxy reachability itself is no longer
// verified or gated on the browser side -- Monti Anty checks the assigned
// proxy before ever spawning this process, so the browser just applies it via
// MontiProfileService::Connect() (fire-and-forget, for the SocksBridge/auth
// plumbing) and navigates straight to the real target page.
constexpr char kMontiUnauthorizedLaunchPath[] =
    "/v1.0/internal/unauthorized-launch";

// Shown when the Monti binary is started directly (double-clicked, run from
// a shell, or invoked by anything other than Monti Anty) instead of through
// a --monti-profile-launch invocation. Every legitimate session is spawned by
// Monti Anty with an assigned proxy and fingerprint already resolved; a
// process that reaches DetermineURLsAndLaunch without those switches has no
// proxy and no spoofed fingerprint applied, so letting it browse normally
// would silently leak the host's real IP/fingerprint. Refuse instead of
// falling back to a plain browsing session.
constexpr char kMontiUnauthorizedLaunchHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>Blocked</title>"
    "<style>body{margin:0;display:grid;min-height:100vh;place-items:center;"
    "background:#fbfaf8;color:#1d1c18;"
    "font-family:system-ui,-apple-system,BlinkMacSystemFont,sans-serif}"
    "main{text-align:center;max-width:440px;padding:0 24px}"
    "h1{font-size:22px;margin:0 0 8px;color:#b3261e}"
    "p{color:#716b62;font-size:15px}</style></head>"
    "<body><main><h1>Direct launch blocked</h1>"
    "<p>Monti Browser was started without going through Monti Anty, so no "
    "proxy or fingerprint protection is in place. Refusing to browse -- open "
    "a profile from Monti Anty instead.</p></main></body></html>";

// "path?query" → (path, query)
std::pair<std::string, std::string> SplitPathQuery(const std::string& raw) {
  auto pos = raw.find('?');
  if (pos == std::string::npos)
    return {raw, {}};
  return {raw.substr(0, pos), raw.substr(pos + 1)};
}

// "/v1.0/browser_profiles/foo/start" → ["v1.0","browser_profiles","foo","start"]
std::vector<std::string> SplitPath(const std::string& path) {
  return base::SplitString(path, "/",
                           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
}

bool HasAutomationParam(const std::string& query) {
  for (const auto& kv : base::SplitString(
           query, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (kv == "automation=1")
      return true;
  }
  return false;
}

base::FilePath ProfileFullPath(const std::string& profile_dir) {
  base::FilePath user_data;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data))
    return {};
  return user_data.Append(base::FilePath::FromUTF8Unsafe(profile_dir));
}

// ---- JSON helpers -----------------------------------------------------------

std::string ToJson(const base::DictValue& d) {
  std::string out;
  base::JSONWriter::Write(base::Value(d.Clone()), &out);
  return out;
}

std::string OkJson(base::DictValue payload) {
  base::DictValue w;
  w.Set("status", true);
  w.Set("data", std::move(payload));
  return ToJson(w);
}

std::string OkListJson(base::ListValue payload) {
  base::DictValue w;
  w.Set("status", true);
  w.Set("data", std::move(payload));
  return ToJson(w);
}

std::string ErrorJson(std::string_view msg) {
  base::DictValue d;
  d.Set("status", false);
  d.Set("msg", msg);
  return ToJson(d);
}

// ---- Cookie serialisation ---------------------------------------------------

base::DictValue CookieToDict(const net::CanonicalCookie& c) {
  base::DictValue d;
  d.Set("name", c.Name());
  d.Set("value", c.Value());
  d.Set("domain", c.Domain());
  d.Set("path", c.Path());
  d.Set("expires", c.ExpiryDate().is_null()
                       ? -1.0
                       : c.ExpiryDate().InSecondsFSinceUnixEpoch());
  d.Set("httpOnly", c.IsHttpOnly());
  d.Set("secure", c.IsSecure());
  d.Set("session", !c.IsPersistent());
  switch (c.SameSite()) {
    case net::CookieSameSite::STRICT_MODE:
      d.Set("sameSite", "Strict");
      break;
    case net::CookieSameSite::LAX_MODE:
      d.Set("sameSite", "Lax");
      break;
    default:
      d.Set("sameSite", "None");
      break;
  }
  return d;
}

// Returns nullptr when the dict is missing required fields or sanitisation
// fails; the caller should skip that cookie.
std::unique_ptr<net::CanonicalCookie> DictToCanonicalCookie(
    const base::DictValue& d) {
  const std::string* name = d.FindString("name");
  const std::string* value = d.FindString("value");
  const std::string* domain = d.FindString("domain");
  const std::string* path = d.FindString("path");
  if (!name || !value || !domain || !path)
    return nullptr;

  std::optional<double> exp = d.FindDouble("expires");
  base::Time expiry;
  if (exp && *exp > 0)
    expiry = base::Time::FromSecondsSinceUnixEpoch(*exp);

  bool secure = d.FindBool("secure").value_or(false);
  bool http_only = d.FindBool("httpOnly").value_or(false);

  net::CookieSameSite same_site = net::CookieSameSite::NO_RESTRICTION;
  if (const std::string* ss = d.FindString("sameSite")) {
    std::string lower = base::ToLowerASCII(*ss);
    if (lower == "strict")
      same_site = net::CookieSameSite::STRICT_MODE;
    else if (lower == "lax")
      same_site = net::CookieSameSite::LAX_MODE;
  }

  // Build source URL from the domain so CreateSanitizedCookie can validate.
  std::string bare = *domain;
  if (!bare.empty() && bare[0] == '.')
    bare = bare.substr(1);
  GURL source((secure ? "https://" : "http://") + bare);
  if (!source.is_valid())
    return nullptr;

  net::CookieInclusionStatus status;
  return net::CanonicalCookie::CreateSanitizedCookie(
      source, *name, *value, *domain, *path,
      /*creation=*/base::Time::Now(), expiry,
      /*last_access=*/base::Time::Now(),
      secure, http_only, same_site,
      net::COOKIE_PRIORITY_DEFAULT,
      /*partition_key=*/std::nullopt,
      &status);
}

}  // namespace

// static
MontiLocalApiServer* MontiLocalApiServer::GetInstance() {
  static base::NoDestructor<MontiLocalApiServer> instance;
  return instance.get();
}

MontiLocalApiServer::MontiLocalApiServer()
    : io_runner_(content::GetIOThreadTaskRunner({})),
      ui_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {}

MontiLocalApiServer::~MontiLocalApiServer() = default;

void MontiLocalApiServer::Start(uint16_t port) {
  DCHECK(ui_runner_->BelongsToCurrentThread());
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::StartOnIOThread,
                     base::Unretained(this), port));
}

void MontiLocalApiServer::Stop() {
  DCHECK(ui_runner_->BelongsToCurrentThread());
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::StopOnIOThread,
                     base::Unretained(this)));
}

// IO thread.
void MontiLocalApiServer::StartOnIOThread(uint16_t port) {
  if (http_server_)
    return;
  auto socket = std::make_unique<net::TCPServerSocket>(
      /*net_log=*/nullptr, net::NetLogSource());
  if (int rv = socket->ListenWithAddressAndPort("127.0.0.1", port, kBacklog);
      rv != net::OK) {
    LOG(ERROR) << "MontiLocalApi: cannot bind 127.0.0.1:" << port
               << " (err " << rv << ")";
    return;
  }
  http_server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  LOG(INFO) << "MontiLocalApi: listening on 127.0.0.1:" << port;
}

// IO thread.
void MontiLocalApiServer::StopOnIOThread() {
  http_server_.reset();
}

// ---- net::HttpServer::Delegate (IO thread) ----------------------------------

void MontiLocalApiServer::OnConnect(int /*connection_id*/) {}

void MontiLocalApiServer::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  // Handle CORS preflight without a round-trip to the UI thread.
  if (info.method == "OPTIONS") {
    net::HttpServerResponseInfo resp(net::HTTP_OK);
    resp.AddHeader("Access-Control-Allow-Origin", "*");
    resp.AddHeader("Access-Control-Allow-Methods",
                   "GET, POST, PATCH, DELETE, OPTIONS");
    resp.AddHeader("Access-Control-Allow-Headers", "Content-Type");
    resp.SetBody("", "text/plain");
    http_server_->SendResponse(connection_id, resp, kLocalApiAnnotation);
    return;
  }

  auto [path, query] = SplitPathQuery(info.path);

  if (path == kMontiUnauthorizedLaunchPath) {
    net::HttpServerResponseInfo resp(net::HTTP_OK);
    resp.SetBody(kMontiUnauthorizedLaunchHtml, "text/html; charset=utf-8");
    http_server_->SendResponse(connection_id, resp, kLocalApiAnnotation);
    return;
  }

  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::DispatchOnUIThread,
                     base::Unretained(this), connection_id,
                     info.method, std::move(path), std::move(query),
                     info.data));
}

void MontiLocalApiServer::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& /*info*/) {
  http_server_->Close(connection_id);
}

void MontiLocalApiServer::OnWebSocketMessage(int /*connection_id*/,
                                             std::string /*data*/) {}

void MontiLocalApiServer::OnClose(int /*connection_id*/) {}

// ---- IO thread send helpers -------------------------------------------------

void MontiLocalApiServer::SendJsonOnIOThread(int connection_id,
                                             int http_status,
                                             std::string json) {
  if (!http_server_)
    return;
  net::HttpServerResponseInfo resp(
      static_cast<net::HttpStatusCode>(http_status));
  resp.AddHeader("Access-Control-Allow-Origin", "*");
  resp.SetBody(std::move(json), "application/json");
  http_server_->SendResponse(connection_id, resp, kLocalApiAnnotation);
}

void MontiLocalApiServer::SendErrorOnIOThread(int connection_id,
                                              int http_status,
                                              std::string_view message) {
  SendJsonOnIOThread(connection_id, http_status, ErrorJson(message));
}

// ---- UI thread: request dispatch --------------------------------------------

void MontiLocalApiServer::DispatchOnUIThread(int connection_id,
                                             std::string method,
                                             std::string path,
                                             std::string query,
                                             std::string body) {
  std::vector<std::string> s = SplitPath(path);
  // s[0]="v1.0"  s[1]="browser_profiles"|"proxy"  s[2]=id  s[3]=action

  if (s.size() >= 2 && s[1] == "browser_profiles") {
    if (s.size() == 2) {
      if (method == "GET")  return HandleGetProfiles(connection_id);
      if (method == "POST") {
        auto v = base::JSONReader::ReadDict(body, 0);
        return HandleCreateProfile(connection_id,
                                   v ? std::move(*v) : base::DictValue());
      }
    }
    if (s.size() == 3) {
      if (method == "GET")    return HandleGetProfile(connection_id, s[2]);
      if (method == "PATCH") {
        auto v = base::JSONReader::ReadDict(body, 0);
        return HandleUpdateProfile(connection_id, s[2],
                                   v ? std::move(*v) : base::DictValue());
      }
      if (method == "DELETE") return HandleDeleteProfileById(connection_id, s[2]);
    }
    if (s.size() == 4) {
      const std::string& id     = s[2];
      const std::string& action = s[3];
      if (action == "start")   return HandleStartProfile(connection_id, id,
                                          HasAutomationParam(query));
      if (action == "stop")    return HandleStopProfile(connection_id, id);
      if (action == "debug")   return HandleGetProfileDebug(connection_id, id);
      if (action == "cookies") {
        if (method == "GET")    return HandleGetCookies(connection_id, id);
        if (method == "DELETE") return HandleDeleteCookies(connection_id, id);
        if (method == "POST") {
          auto v = base::JSONReader::Read(body, 0);
          return HandlePostCookies(connection_id, id,
                                   v ? std::move(*v) : base::Value());
        }
      }
    }
  }

  if (s.size() >= 2 && s[1] == "proxy") {
    if (s.size() == 2) {
      if (method == "GET")  return HandleGetProxies(connection_id);
      if (method == "POST") {
        auto v = base::JSONReader::ReadDict(body, 0);
        return HandlePostProxy(connection_id,
                               v ? std::move(*v) : base::DictValue());
      }
    }
    if (s.size() == 3) {
      if (method == "PATCH") {
        auto v = base::JSONReader::ReadDict(body, 0);
        return HandlePatchProxy(connection_id, s[2],
                                v ? std::move(*v) : base::DictValue());
      }
      if (method == "DELETE") return HandleDeleteProxyById(connection_id, s[2]);
    }
  }

  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                     base::Unretained(this), connection_id, 404, "Not found"));
}

// ---- Serialisation ----------------------------------------------------------

base::DictValue MontiLocalApiServer::ProfileEntryToDict(
    const MontiProfileEntry& e) const {
  base::DictValue d;
  d.Set("id", e.id);
  d.Set("name", e.name);
  d.Set("running", MontiManager::GetInstance()->IsRunning(e.id));
  d.Set("profile_status", e.profile_status);
  d.Set("color", e.color);
  d.Set("folder_id", e.folder_id);
  d.Set("notes", e.notes);
  d.Set("created_at", e.created_at);
  d.Set("ua_preset", e.ua_preset);

  base::ListValue tags;
  for (const auto& t : e.tags)
    tags.Append(t);
  d.Set("tags", std::move(tags));

  if (!e.assigned_proxy_id.empty()) {
    if (const MontiProxy* p =
            ProxyStore::GetInstance()->GetById(e.assigned_proxy_id)) {
      d.Set("proxy", ProxyToDict(*p));
    }
  }

  if (!e.fingerprint.empty()) {
    const Fingerprint fp = FromJson(e.fingerprint);
    base::DictValue fp_dict;
    fp_dict.Set("platform", fp.platform);
    fp_dict.Set("ua_string", fp.ua_string);
    fp_dict.Set("webrtc_mode", fp.webrtc_mode);
    fp_dict.Set("canvas_mode", fp.canvas_mode);
    fp_dict.Set("webgl_mode", fp.webgl_mode);
    fp_dict.Set("webgpu_mode", fp.webgpu_mode);
    fp_dict.Set("client_rects_mode", fp.client_rects_mode);
    fp_dict.Set("audio_mode", fp.audio_mode);
    fp_dict.Set("timezone", fp.timezone);
    fp_dict.Set("geolocation_mode", fp.geolocation_mode);
    fp_dict.Set("cpu_cores", fp.cpu_cores);
    fp_dict.Set("memory_gb", fp.memory_gb);
    fp_dict.Set("screen", fp.screen);
    fp_dict.Set("do_not_track", fp.do_not_track);
    d.Set("fingerprint", std::move(fp_dict));
  }

  return d;
}

base::DictValue MontiLocalApiServer::ProxyToDict(
    const MontiProxy& p) const {
  base::DictValue d;
  d.Set("id", p.id);
  d.Set("name", p.name);
  d.Set("host", p.host);
  d.Set("http_port", p.http_port);
  d.Set("socks_port", p.socks_port);
  d.Set("username", p.username);
  d.Set("country", p.country);
  d.Set("transport", p.transport);
  d.Set("status", p.status);
  d.Set("assigned_profile", p.assigned_profile);
  return d;
}

// ---- Reply helpers ----------------------------------------------------------

void MontiLocalApiServer::ReplyWithProfiles(int conn) {
  base::ListValue list;
  for (const auto& e : ProfileStore::GetInstance()->list())
    list.Append(ProfileEntryToDict(e));
  std::string json = OkListJson(std::move(list));
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, std::move(json)));
}

void MontiLocalApiServer::ReplyWithProxies(int conn) {
  base::ListValue list;
  for (const auto& p : ProxyStore::GetInstance()->list())
    list.Append(ProxyToDict(p));
  std::string json = OkListJson(std::move(list));
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, std::move(json)));
}

// ---- Profile handlers -------------------------------------------------------

void MontiLocalApiServer::HandleGetProfiles(int conn) {
  ReplyWithProfiles(conn);
}

void MontiLocalApiServer::HandleGetProfile(int conn, std::string id) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200,
                     OkJson(ProfileEntryToDict(*e))));
}

void MontiLocalApiServer::HandleCreateProfile(int conn,
                                              base::DictValue body) {
  ProfileDraft draft;
  if (const std::string* v = body.FindString("name"))       draft.name = *v;
  if (const std::string* v = body.FindString("proxy_id"))   draft.proxy_id = *v;
  if (const std::string* v = body.FindString("folder_id"))  draft.folder_id = *v;
  if (const std::string* v = body.FindString("status"))     draft.status = *v;
  if (const std::string* v = body.FindString("color"))      draft.color = *v;
  if (const base::ListValue* tags = body.FindList("tags")) {
    for (const auto& t : *tags)
      if (t.is_string()) draft.tags.push_back(t.GetString());
  }
  if (const std::string* preset = body.FindString("platform")) {
    draft.fingerprint =
        Generate(*preset, static_cast<uint32_t>(base::RandUint64()));
  }

  MontiManager::GetInstance()->CreateProfileFull(
      std::move(draft),
      base::BindOnce(&MontiLocalApiServer::OnCreateProfileDone,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnCreateProfileDone(int conn,
                                              bool ok,
                                              const std::string& msg) {
  if (!ok) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 400, msg));
    return;
  }
  ReplyWithProfiles(conn);
}

void MontiLocalApiServer::HandleUpdateProfile(int conn,
                                              std::string id,
                                              base::DictValue body) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  ProfileDraft draft;
  draft.name =
      body.FindString("name") ? *body.FindString("name") : e->name;
  draft.proxy_id =
      body.FindString("proxy_id") ? *body.FindString("proxy_id")
                                  : e->assigned_proxy_id;
  draft.folder_id =
      body.FindString("folder_id") ? *body.FindString("folder_id")
                                   : e->folder_id;
  draft.color =
      body.FindString("color") ? *body.FindString("color") : e->color;
  draft.status =
      body.FindString("status") ? *body.FindString("status")
                                : e->profile_status;
  if (const base::ListValue* tags = body.FindList("tags")) {
    for (const auto& t : *tags)
      if (t.is_string()) draft.tags.push_back(t.GetString());
  } else {
    draft.tags = e->tags;
  }

  draft.fingerprint = FromJson(e->fingerprint);
  if (const std::string* preset = body.FindString("platform")) {
    if (!preset->empty() && *preset != draft.fingerprint.preset) {
      Fingerprint fresh =
          Generate(*preset, static_cast<uint32_t>(base::RandUint64()));
      draft.fingerprint.preset     = fresh.preset;
      draft.fingerprint.platform   = fresh.platform;
      draft.fingerprint.ua_string  = fresh.ua_string;
    }
  }

  MontiManager::GetInstance()->UpdateProfileFull(
      id, std::move(draft),
      base::BindOnce(&MontiLocalApiServer::OnUpdateProfileDone,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnUpdateProfileDone(int conn,
                                              bool ok,
                                              const std::string& msg) {
  if (!ok) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 400, msg));
    return;
  }
  ReplyWithProfiles(conn);
}

void MontiLocalApiServer::HandleDeleteProfileById(int conn, std::string id) {
  MontiManager::GetInstance()->DeleteProfile(
      id,
      base::BindOnce(&MontiLocalApiServer::OnDeleteProfileDone,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnDeleteProfileDone(int conn,
                                              bool ok,
                                              const std::string& msg) {
  if (!ok) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 400, msg));
    return;
  }
  ReplyWithProfiles(conn);
}

void MontiLocalApiServer::HandleStartProfile(int conn,
                                             std::string id,
                                             bool /*automation*/) {
  MontiManager::GetInstance()->LaunchProfile(
      id,
      base::BindOnce(&MontiLocalApiServer::OnLaunchProfileDone,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnLaunchProfileDone(
    int conn,
    MontiManager::LaunchOutcome outcome,
    const std::string& msg) {
  if (outcome != MontiManager::LaunchOutcome::kLaunched) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 400, msg));
    return;
  }
  // Return minimal automation block (full per-profile CDP is not available in
  // single-process mode; use --remote-debugging-port for global CDP access).
  base::DictValue resp;
  resp.Set("status", true);
  resp.Set("msg", msg);
  base::DictValue automation;
  automation.Set("port", 0);
  automation.Set("wsEndpoint", "");
  resp.Set("automation", std::move(automation));
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, ToJson(resp)));
}

void MontiLocalApiServer::HandleStopProfile(int conn, std::string id) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  base::FilePath profile_path = ProfileFullPath(e->profile_dir);
  if (Profile* loaded =
          g_browser_process->profile_manager()->GetProfileByPath(
              profile_path)) {
    chrome::CloseAllBrowsersWithProfile(loaded);
  }

  base::DictValue resp;
  resp.Set("status", true);
  resp.Set("msg", "Profile stopped.");
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, ToJson(resp)));
}

void MontiLocalApiServer::HandleGetProfileDebug(int conn, std::string id) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(id);
  Profile* loaded = nullptr;
  if (e) {
    loaded = g_browser_process->profile_manager()->GetProfileByPath(
        ProfileFullPath(e->profile_dir));
  } else {
    // No ProfileStore record: this may still be a legitimate external-launcher
    // (--monti-profile-launch) session that was never created through this
    // manager -- MontiManager::OnBrowserCreated applies a runtime-only
    // identity for exactly this case. Each such process is single-profile, so
    // if this process's own --monti-profile-id matches, report on whichever
    // profile is actually loaded.
    const base::CommandLine& command_line =
        *base::CommandLine::ForCurrentProcess();
    if (command_line.HasSwitch("monti-profile-launch") &&
        command_line.GetSwitchValueASCII("monti-profile-id") == id) {
      std::vector<Profile*> loaded_profiles =
          g_browser_process->profile_manager()->GetLoadedProfiles();
      if (!loaded_profiles.empty()) {
        loaded = loaded_profiles.front();
      }
    }
  }
  if (!e && !loaded) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  base::DictValue payload;
  payload.Set("id", id);
  payload.Set("running", loaded != nullptr);
  if (loaded) {
    MontiProfileService* service =
        MontiProfileServiceFactory::GetForProfile(loaded);
    payload.Set("fingerprint", service->fingerprint());
    const char* state = "direct";
    switch (service->connection_state()) {
      case MontiProfileService::ConnectionState::kConnecting:
        state = "connecting";
        break;
      case MontiProfileService::ConnectionState::kConnected:
        state = "connected";
        break;
      case MontiProfileService::ConnectionState::kDirect:
        state = "direct";
        break;
    }
    payload.Set("connection_state", state);
    payload.Set("connected_host", service->connected_host());
    payload.Set("connected_port", service->connected_port());
    payload.Set("connected_proxy_id", service->connected_proxy_id());
    payload.Set("connected_country", service->connected_country());
    payload.Set("connected_egress_ip", service->connected_egress_ip());
  }
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, OkJson(std::move(payload))));
}

// ---- Cookie handlers --------------------------------------------------------

void MontiLocalApiServer::HandleGetCookies(int conn, std::string profile_id) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(profile_id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  base::FilePath path = ProfileFullPath(e->profile_dir);
  ProfileManager* pm = g_browser_process->profile_manager();
  if (Profile* loaded = pm->GetProfileByPath(path)) {
    OnProfileLoadedForCookieGet(conn, loaded);
    return;
  }
  pm->CreateProfileAsync(
      path,
      base::BindOnce(&MontiLocalApiServer::OnProfileLoadedForCookieGet,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnProfileLoadedForCookieGet(int conn,
                                                      Profile* profile) {
  if (!profile) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 500,
                       "Failed to load profile"));
    return;
  }
  profile->GetDefaultStoragePartition()
      ->GetCookieManagerForBrowserProcess()
      ->GetAllCookies(
          base::BindOnce(&MontiLocalApiServer::OnAllCookiesReceived,
                         weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnAllCookiesReceived(int conn,
                                               const net::CookieList& cookies) {
  base::ListValue list;
  for (const auto& c : cookies)
    list.Append(CookieToDict(c));
  std::string json = OkListJson(std::move(list));
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, std::move(json)));
}

void MontiLocalApiServer::HandlePostCookies(int conn,
                                            std::string profile_id,
                                            base::Value cookies_value) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(profile_id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  base::FilePath path = ProfileFullPath(e->profile_dir);
  ProfileManager* pm = g_browser_process->profile_manager();
  if (Profile* loaded = pm->GetProfileByPath(path)) {
    OnProfileLoadedForCookieSet(conn, std::move(cookies_value), loaded);
    return;
  }
  pm->CreateProfileAsync(
      path,
      base::BindOnce(&MontiLocalApiServer::OnProfileLoadedForCookieSet,
                     weak_factory_.GetWeakPtr(), conn,
                     std::move(cookies_value)));
}

void MontiLocalApiServer::OnProfileLoadedForCookieSet(
    int conn,
    base::Value cookies_value,
    Profile* profile) {
  if (!profile) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 500,
                       "Failed to load profile"));
    return;
  }
  if (!cookies_value.is_list()) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 400,
                       "Body must be a JSON array of cookie objects"));
    return;
  }

  network::mojom::CookieManager* cm =
      profile->GetDefaultStoragePartition()
          ->GetCookieManagerForBrowserProcess();

  int imported = 0;
  int failed = 0;
  for (const auto& item : cookies_value.GetList()) {
    if (!item.is_dict()) { ++failed; continue; }
    auto cookie = DictToCanonicalCookie(item.GetDict());
    if (!cookie) { ++failed; continue; }

    std::string domain = cookie->Domain();
    if (!domain.empty() && domain[0] == '.') domain = domain.substr(1);
    GURL source_url((cookie->IsSecure() ? "https://" : "http://") + domain);

    cm->SetCanonicalCookie(
        *cookie, source_url,
        net::CookieOptions::MakeAllInclusive(),
        base::BindOnce([](net::CookieAccessResult) {}));
    ++imported;
  }

  base::DictValue resp;
  resp.Set("status", true);
  resp.Set("imported", imported);
  resp.Set("failed", failed);
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, ToJson(resp)));
}

void MontiLocalApiServer::HandleDeleteCookies(int conn,
                                              std::string profile_id) {
  const MontiProfileEntry* e = ProfileStore::GetInstance()->GetById(profile_id);
  if (!e) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Profile not found"));
    return;
  }

  base::FilePath path = ProfileFullPath(e->profile_dir);
  ProfileManager* pm = g_browser_process->profile_manager();
  if (Profile* loaded = pm->GetProfileByPath(path)) {
    OnProfileLoadedForCookieDelete(conn, loaded);
    return;
  }
  pm->CreateProfileAsync(
      path,
      base::BindOnce(&MontiLocalApiServer::OnProfileLoadedForCookieDelete,
                     weak_factory_.GetWeakPtr(), conn));
}

void MontiLocalApiServer::OnProfileLoadedForCookieDelete(int conn,
                                                         Profile* profile) {
  if (!profile) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 500,
                       "Failed to load profile"));
    return;
  }
  // Empty filter = delete everything.
  profile->GetDefaultStoragePartition()
      ->GetCookieManagerForBrowserProcess()
      ->DeleteCookies(network::mojom::CookieDeletionFilter::New(),
                      base::BindOnce([](uint32_t) {}));

  base::DictValue resp;
  resp.Set("status", true);
  resp.Set("msg", "All cookies cleared");
  io_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&MontiLocalApiServer::SendJsonOnIOThread,
                     base::Unretained(this), conn, 200, ToJson(resp)));
}

// ---- Proxy handlers ---------------------------------------------------------

void MontiLocalApiServer::HandleGetProxies(int conn) {
  ReplyWithProxies(conn);
}

void MontiLocalApiServer::HandlePostProxy(int conn, base::DictValue body) {
  MontiProxy p;
  p.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  if (const std::string* v = body.FindString("host"))      p.host = *v;
  if (const std::string* v = body.FindString("name"))      p.name = *v;
  if (const std::string* v = body.FindString("username"))  p.username = *v;
  if (const std::string* v = body.FindString("password"))  p.password = *v;
  if (const std::string* v = body.FindString("country"))   p.country = *v;
  if (const std::string* v = body.FindString("transport")) p.transport = *v;
  if (std::optional<int> port = body.FindInt("http_port")) p.http_port = *port;
  if (std::optional<int> port = body.FindInt("socks_port")) p.socks_port = *port;
  if (p.http_port == 0) {
    if (std::optional<int> port = body.FindInt("port")) p.http_port = *port;
  }
  if (p.name.empty())
    p.name = p.country.empty() ? p.host : p.country + " · " + p.host;

  ProxyStore::GetInstance()->Add(p);
  ReplyWithProxies(conn);
}

void MontiLocalApiServer::HandlePatchProxy(int conn,
                                           std::string id,
                                           base::DictValue body) {
  const MontiProxy* existing = ProxyStore::GetInstance()->GetById(id);
  if (!existing) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Proxy not found"));
    return;
  }
  MontiProxy p = *existing;
  if (const std::string* v = body.FindString("host"))      p.host = *v;
  if (const std::string* v = body.FindString("name"))      p.name = *v;
  if (const std::string* v = body.FindString("username"))  p.username = *v;
  if (const std::string* v = body.FindString("password"))  p.password = *v;
  if (const std::string* v = body.FindString("country"))   p.country = *v;
  if (const std::string* v = body.FindString("transport")) p.transport = *v;
  if (std::optional<int> port = body.FindInt("http_port")) p.http_port = *port;
  if (std::optional<int> port = body.FindInt("socks_port")) p.socks_port = *port;
  ProxyStore::GetInstance()->Update(p);
  ReplyWithProxies(conn);
}

void MontiLocalApiServer::HandleDeleteProxyById(int conn, std::string id) {
  if (!ProxyStore::GetInstance()->GetById(id)) {
    io_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MontiLocalApiServer::SendErrorOnIOThread,
                       base::Unretained(this), conn, 404, "Proxy not found"));
    return;
  }
  ProxyStore::GetInstance()->Remove(id);
  ReplyWithProxies(conn);
}

}  // namespace monti

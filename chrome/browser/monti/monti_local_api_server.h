// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_LOCAL_API_SERVER_H_
#define CHROME_BROWSER_MONTI_MONTI_LOCAL_API_SERVER_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "chrome/browser/monti/monti_manager.h"
#include "net/cookies/canonical_cookie.h"
#include "net/server/http_server.h"

class Profile;

namespace monti {

// Local REST API server bound to 127.0.0.1:39217. Exposes a Dolphin
// Anty-compatible endpoint layout so external automation scripts can manage
// Monti profiles, proxies, and cookies over plain HTTP without any UI.
//
// Endpoints:
//   GET    /v1.0/browser_profiles               – list all profiles
//   GET    /v1.0/browser_profiles/:id            – single profile JSON
//   POST   /v1.0/browser_profiles               – create profile
//   PATCH  /v1.0/browser_profiles/:id            – update metadata / proxy
//   DELETE /v1.0/browser_profiles/:id            – delete profile
//   GET    /v1.0/browser_profiles/:id/start      – open window
//   GET    /v1.0/browser_profiles/:id/stop       – close all windows
//   GET    /v1.0/browser_profiles/:id/cookies    – export cookies as JSON
//   POST   /v1.0/browser_profiles/:id/cookies    – import JSON cookie array
//   DELETE /v1.0/browser_profiles/:id/cookies    – clear all cookies
//
//   GET    /v1.0/proxy         – list proxies
//   POST   /v1.0/proxy         – add proxy
//   PATCH  /v1.0/proxy/:id     – update proxy
//   DELETE /v1.0/proxy/:id     – delete proxy
//
// Threading:
//   net::HttpServer lives on the browser IO thread.  All MontiManager /
//   ProfileStore / ProxyStore calls must run on the UI thread, so request
//   handling is posted there and responses are posted back to the IO thread.
class MontiLocalApiServer : public net::HttpServer::Delegate {
 public:
  static MontiLocalApiServer* GetInstance();

  MontiLocalApiServer(const MontiLocalApiServer&) = delete;
  MontiLocalApiServer& operator=(const MontiLocalApiServer&) = delete;

  // Starts the TCP listener. Called from the UI thread at browser startup.
  // No-op if already running.
  void Start(uint16_t port = 39217);

  // Shuts down the listener. Called from the UI thread.
  void Stop();

  // net::HttpServer::Delegate (called on the IO thread):
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

 private:
  friend class base::NoDestructor<MontiLocalApiServer>;

  MontiLocalApiServer();
  ~MontiLocalApiServer() override;

  // IO thread: create / destroy the TCP listener + HttpServer.
  void StartOnIOThread(uint16_t port);
  void StopOnIOThread();

  // IO thread: emit a JSON HTTP response.
  void SendJsonOnIOThread(int connection_id, int http_status, std::string json);
  void SendErrorOnIOThread(int connection_id,
                           int http_status,
                           std::string_view message);

  // UI thread: parse the request path and dispatch to a handler.
  void DispatchOnUIThread(int connection_id,
                          std::string method,
                          std::string path,
                          std::string query,
                          std::string body);

  // --- Profile handlers (UI thread) ---
  void HandleGetProfiles(int conn);
  void HandleGetProfile(int conn, std::string id);
  void HandleCreateProfile(int conn, base::DictValue body);
  void HandleUpdateProfile(int conn, std::string id, base::DictValue body);
  void HandleDeleteProfileById(int conn, std::string id);
  void HandleStartProfile(int conn, std::string id, bool automation);
  void HandleStopProfile(int conn, std::string id);
  // Diagnostic snapshot for a launched profile: the fingerprint currently
  // applied to it and its proxy connection state (host/port/proxy id/egress
  // IP), read straight off MontiProfileService. Used by the launcher's
  // end-to-end verify script and by anyone debugging a launch by hand.
  void HandleGetProfileDebug(int conn, std::string id);

  // --- Cookie handlers (UI thread) ---
  void HandleGetCookies(int conn, std::string profile_id);
  void HandlePostCookies(int conn,
                         std::string profile_id,
                         base::Value cookies_value);
  void HandleDeleteCookies(int conn, std::string profile_id);

  // --- Proxy handlers (UI thread) ---
  void HandleGetProxies(int conn);
  void HandlePostProxy(int conn, base::DictValue body);
  void HandlePatchProxy(int conn, std::string id, base::DictValue body);
  void HandleDeleteProxyById(int conn, std::string id);

  // MontiManager async trampolines (UI thread):
  void OnCreateProfileDone(int conn, bool ok, const std::string& msg);
  void OnUpdateProfileDone(int conn, bool ok, const std::string& msg);
  void OnDeleteProfileDone(int conn, bool ok, const std::string& msg);
  void OnLaunchProfileDone(int conn,
                           MontiManager::LaunchOutcome outcome,
                           const std::string& msg);

  // Cookie async chain (UI thread):
  void OnProfileLoadedForCookieGet(int conn, Profile* profile);
  void OnAllCookiesReceived(int conn, const net::CookieList& cookies);
  void OnProfileLoadedForCookieSet(int conn,
                                   base::Value cookies_value,
                                   Profile* profile);
  void OnProfileLoadedForCookieDelete(int conn, Profile* profile);

  // Serialisation helpers (UI thread).
  base::DictValue ProfileEntryToDict(const MontiProfileEntry& e) const;
  base::DictValue ProxyToDict(const MontiProxy& p) const;

  // Sends back the full profile / proxy list (used after mutating operations).
  void ReplyWithProfiles(int conn);
  void ReplyWithProxies(int conn);

  scoped_refptr<base::SingleThreadTaskRunner> io_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> ui_runner_;

  // Lives on io_runner_; null when stopped.
  std::unique_ptr<net::HttpServer> http_server_;

  base::WeakPtrFactory<MontiLocalApiServer> weak_factory_{this};
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_LOCAL_API_SERVER_H_

// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/csv_importer.h"

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace monti {

namespace {

// Column order in the provided CSV (the original, name-less schema). When a
// leading `name` column is present every index below is shifted by +1.
enum Column {
  kId = 0,
  kIp = 1,
  kPortHttp = 2,
  kPortSocks5 = 3,
  kUsername = 4,
  kPassword = 5,
  kInternalIp = 6,
  kCountry = 7,
  kColumnCount = 8,
};

}  // namespace

std::vector<MontiProxy> ImportProxiesFromCsvString(const std::string& text) {
  std::vector<MontiProxy> proxies;

  std::string_view contents(text);

  // Strip a UTF-8 byte-order mark if present.
  static constexpr std::string_view kBom = "\xEF\xBB\xBF";
  if (base::StartsWith(contents, kBom)) {
    contents = contents.substr(kBom.size());
  }

  std::vector<std::string_view> lines = base::SplitStringPiece(
      contents, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  bool header_handled = false;
  bool has_name = false;  // first column is `name` rather than `id`.
  for (std::string_view line : lines) {
    // Inspect the first row to detect the schema and skip the header. A
    // `name;`-prefixed header marks the extended (export) layout; an `id;`
    // header is the original layout; anything else is treated as data in the
    // original layout.
    if (!header_handled) {
      header_handled = true;
      if (base::StartsWith(line, "name;",
                           base::CompareCase::INSENSITIVE_ASCII)) {
        has_name = true;
        continue;
      }
      if (base::StartsWith(line, "id;",
                           base::CompareCase::INSENSITIVE_ASCII)) {
        continue;
      }
    }

    const size_t offset = has_name ? 1 : 0;
    std::vector<std::string_view> fields = base::SplitStringPiece(
        line, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    if (fields.size() < kColumnCount + offset) {
      continue;
    }

    MontiProxy proxy;
    if (has_name) {
      proxy.name = std::string(fields[0]);
    }
    proxy.id = std::string(fields[kId + offset]);
    proxy.host = std::string(fields[kIp + offset]);
    base::StringToInt(fields[kPortHttp + offset], &proxy.http_port);
    base::StringToInt(fields[kPortSocks5 + offset], &proxy.socks_port);
    proxy.username = std::string(fields[kUsername + offset]);
    proxy.password = std::string(fields[kPassword + offset]);
    proxy.country = std::string(fields[kCountry + offset]);

    // Reject only when no usable endpoint exists. A SOCKS-only row (http_port
    // == 0 but socks_port > 0) is valid and must not be silently dropped.
    if (proxy.host.empty() || (proxy.http_port == 0 && proxy.socks_port == 0)) {
      continue;
    }
    // Auto-detect the transport from the ports present. Dolphin rows carry a
    // SOCKS5 port, which routes through the proven authenticated SocksBridge;
    // fall back to HTTP only when no SOCKS port is available.
    proxy.transport = proxy.socks_port > 0 ? "socks5" : "http";
    proxies.push_back(std::move(proxy));
  }

  return proxies;
}

namespace {

// Parses a single Dolphin-style endpoint line (no ';' delimiter), as exported
// in the .xlsx "Proxy" column. Accepts, in order of preference:
//   user:pass@host:port
//   host:port@user:pass
//   host:port:user:pass
//   host:port
// The single port is treated as a SOCKS5 endpoint (Dolphin's "Proxy type" is
// socks5 and the SocksBridge handles its RFC1929 auth); the transport can be
// overridden per-proxy in the UI. Returns false if no host:port is found.
bool ParseDolphinProxyLine(std::string_view line, MontiProxy* out) {
  std::string host_port;
  std::string user_pass;

  const size_t at = line.find('@');
  if (at != std::string_view::npos) {
    std::string_view lhs = line.substr(0, at);
    std::string_view rhs = line.substr(at + 1);
    // Dolphin writes user:pass@host:port. Detect which side is host:port by
    // testing whether its second ':'-field parses as a port number.
    std::vector<std::string_view> rhs_parts = base::SplitStringPiece(
        rhs, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    int probe = 0;
    if (rhs_parts.size() == 2 && base::StringToInt(rhs_parts[1], &probe)) {
      host_port = std::string(rhs);
      user_pass = std::string(lhs);
    } else {
      host_port = std::string(lhs);
      user_pass = std::string(rhs);
    }
  } else {
    std::vector<std::string_view> parts = base::SplitStringPiece(
        line, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() >= 4) {
      // host:port:user:pass
      host_port = base::StrCat({parts[0], ":", parts[1]});
      user_pass = base::StrCat({parts[2], ":", parts[3]});
    } else {
      // host:port
      host_port = std::string(line);
    }
  }

  std::vector<std::string_view> hp = base::SplitStringPiece(
      host_port, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  int port = 0;
  if (hp.size() != 2 || hp[0].empty() || !base::StringToInt(hp[1], &port) ||
      port <= 0) {
    return false;
  }

  out->host = std::string(hp[0]);
  out->socks_port = port;
  out->transport = "socks5";
  if (!user_pass.empty()) {
    const size_t colon = user_pass.find(':');
    if (colon != std::string::npos) {
      out->username = user_pass.substr(0, colon);
      out->password = user_pass.substr(colon + 1);
    } else {
      out->username = user_pass;
    }
  }
  out->id = base::StrCat(
      {"paste:", out->host, ":", base::NumberToString(out->socks_port)});
  return true;
}

}  // namespace

std::vector<MontiProxy> ParseProxiesFromText(const std::string& text) {
  std::string_view contents(text);
  static constexpr std::string_view kBom = "\xEF\xBB\xBF";
  if (base::StartsWith(contents, kBom)) {
    contents = contents.substr(kBom.size());
  }

  // If the blob uses the ';' CSV delimiter, parse it as the structured Dolphin
  // export. Otherwise treat each line as a bare endpoint (the .xlsx "Proxy"
  // column shape).
  if (contents.find(';') != std::string_view::npos) {
    return ImportProxiesFromCsvString(text);
  }

  std::vector<MontiProxy> proxies;
  std::vector<std::string_view> lines = base::SplitStringPiece(
      contents, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (std::string_view line : lines) {
    MontiProxy proxy;
    if (ParseDolphinProxyLine(line, &proxy)) {
      proxies.push_back(std::move(proxy));
    }
  }
  return proxies;
}

std::vector<MontiProxy> ImportProxiesFromCsv(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return {};
  }
  return ImportProxiesFromCsvString(contents);
}

std::string ExportProxiesToCsv(const std::vector<MontiProxy>& proxies) {
  std::string out =
      "name;id;ip;port_http;port_socks5;username;password;internal_ip;country\n";
  for (const MontiProxy& proxy : proxies) {
    base::StrAppend(
        &out,
        {proxy.name, ";", proxy.id, ";", proxy.host, ";",
         base::NumberToString(proxy.http_port), ";",
         base::NumberToString(proxy.socks_port), ";", proxy.username, ";",
         proxy.password, ";", /*internal_ip=*/"", ";", proxy.country, "\n"});
  }
  return out;
}

}  // namespace monti

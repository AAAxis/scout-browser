// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_CSV_IMPORTER_H_
#define CHROME_BROWSER_MONTI_CSV_IMPORTER_H_

#include <string>
#include <vector>

#include "chrome/browser/monti/monti_proxy.h"

namespace base {
class FilePath;
}

namespace monti {

// Parses ';'-delimited proxy CSV text. Tolerates either the original header
// `id;ip;port_http;port_socks5;username;password;internal_ip;country` or the
// extended export header with a leading `name;` column. Skips the BOM and the
// header row.
std::vector<MontiProxy> ImportProxiesFromCsvString(const std::string& text);

// Parses proxy text from either supported shape and returns the parsed entries
// WITHOUT persisting them (used to populate the import-preview modal):
//   * ';'-delimited Dolphin CSV (delegates to ImportProxiesFromCsvString), or
//   * one bare endpoint per line ("user:pass@host:port", as in the .xlsx
//     "Proxy" column), parsed as SOCKS5.
// Transport is auto-detected; ids are generated for paste lines.
std::vector<MontiProxy> ParseProxiesFromText(const std::string& text);

// Reads `path` into a string and parses it via ImportProxiesFromCsvString.
// Returns an empty vector on read failure. Blocking I/O: call off the UI thread.
std::vector<MontiProxy> ImportProxiesFromCsv(const base::FilePath& path);

// Serializes `proxies` to ';'-delimited CSV with the extended header
// `name;id;ip;port_http;port_socks5;username;password;internal_ip;country`
// (the `internal_ip` column is emitted empty). Round-trips with the importer.
std::string ExportProxiesToCsv(const std::vector<MontiProxy>& proxies);

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_CSV_IMPORTER_H_

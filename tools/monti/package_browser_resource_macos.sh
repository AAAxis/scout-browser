#!/usr/bin/env bash
set -euo pipefail

BROWSER_DIR="${1:-out/Release-dmg}"
VERSION="${2:-1.0.0}"
OUT_DIR="${3:-resource-release}"
ARCHIVE_NAME="${4:-Monti-Browser-mac-arm64.zip}"
MANIFEST_NAME="${5:-latest-mac-arm64.json}"

browser_root="$(cd "${BROWSER_DIR}" && pwd)"
app_path=""
for candidate in \
  "${browser_root}/Scout Web.app" \
  "${browser_root}/Monti Browser.app" \
  "${browser_root}/Monti.app" \
  "${browser_root}/Chromium.app"; do
  if [[ -d "${candidate}" ]]; then
    app_path="${candidate}"
    break
  fi
done

if [[ -z "${app_path}" ]]; then
  echo "No browser app found in ${browser_root}. Expected Monti Browser.app, Monti.app, or Chromium.app." >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"
stage="${OUT_DIR}/Monti Browser"
archive_path="${OUT_DIR}/${ARCHIVE_NAME}"
manifest_path="${OUT_DIR}/${MANIFEST_NAME}"

rm -rf "${stage}" "${archive_path}"
mkdir -p "${stage}"
ditto "${app_path}" "${stage}/$(basename "${app_path}")"
ditto -c -k --sequesterRsrc --keepParent "${stage}/$(basename "${app_path}")" "${archive_path}"

sha512="$(openssl dgst -sha512 -binary "${archive_path}" | openssl base64 -A)"
size="$(stat -f '%z' "${archive_path}")"
release_date="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

node - "${manifest_path}" "${VERSION}" "${ARCHIVE_NAME}" "${sha512}" "${size}" "${release_date}" <<'NODE'
const fs = require('fs');
const [manifestPath, version, url, sha512, size, releaseDate] = process.argv.slice(2);
fs.writeFileSync(manifestPath, JSON.stringify({
  version,
  url,
  sha512,
  size: Number(size),
  releaseDate,
}, null, 2));
NODE

echo "Created ${archive_path}"
echo "Created ${manifest_path}"
echo "Upload keys:"
echo "  resources/browser/${ARCHIVE_NAME}"
echo "  resources/browser/${MANIFEST_NAME}"

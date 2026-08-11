# Monti — Release Log

Shipped, distributable builds of the Monti browser. Source for each lives in this
repo's history; the binaries are hosted as GitHub Release assets (not in git).

## v151.0.7906.0 — macOS (Apple Silicon)

- **Platform:** macOS 13.0+ (Ventura and newer), Apple Silicon / arm64 only.
- **Signing:** `Developer ID Application: SIMNETIQ LTD (F7YY9U667A)`, hardened runtime.
- **Notarization:** notarized by Apple **and stapled** — opens with no Gatekeeper warning.
- **Download (public):**
  https://github.com/pochtmanr/monti-downloads/releases/download/v151.0.7906.0/Monti-151.0.7906.0.dmg
- **SHA-256:** `b5772588cb6af822d3ae78ec7a328fff4bc661a1754479407413c03187444d97`
- **Landing CTA:** "Download for Mac" on the hero (repo `rpochtman-lang/browsermontilanding`,
  URL centralized in `landing/lib/env.ts` → `DMG_DOWNLOAD_URL`).

### How this build was produced
1. Non-component release build: `gn gen out/Release-dmg --args='is_debug=false
   is_component_build=false target_cpu="arm64" use_remoteexec=false dcheck_always_on=false
   symbol_level=1'` then `autoninja -C out/Release-dmg chrome`.
2. Sign → DMG → notarize → staple via `package_dmg.sh` (see `../DMG_BUILD.md` at the
   workspace root for the full runbook).

> Distribute the **DMG via a normal download link** — not the raw `.app` through Telegram
> or other sandboxed apps, which stamp an AppSandbox quarantine that blocks Gatekeeper even
> on a notarized app.

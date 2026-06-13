# Sonic the Hedgehog — macOS (Apple Silicon) build

Native arm64 macOS build of Sonic the Hedgehog, attached to release **v0.1.0** as
`SonicTheHedgehogRecomp-macos-arm64.zip`.

## What this is
- The original game statically recompiled to native arm64 (no emulator core shipped).
- Self-contained `.app`: SDL2 bundled via `@executable_path`, ad-hoc codesigned.
- Verified by manual play on Apple Silicon (looks/sounds correct on the golden path).

## Status
Minor audio issue present; otherwise plays correctly. Please report bugs by filing an issue.


## Install
1. Download `SonicTheHedgehogRecomp-macos-arm64.zip` from the **v0.1.0** release and unzip.
2. First launch: right-click `Sonic the Hedgehog.app` -> Open (ad-hoc signed), or
   `xattr -dr com.apple.quarantine "Sonic the Hedgehog.app"`.
3. ROM not included — supply your own dump: Sonic the Hedgehog (Genesis) .bin/.md dump
4. Run: `"Sonic the Hedgehog.app/Contents/MacOS/Sonic the Hedgehog" /path/to/rom`

## Build it yourself
`scripts/release-mac.sh` reproduces this artifact (build -> .app -> zip);
`scripts/release-mac.sh --publish` re-attaches it to the latest release.
Requires: `brew install cmake ninja sdl2 dylibbundler` on Apple Silicon.

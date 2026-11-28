#!/usr/bin/env bash
# Product settings for the shared Genesis macOS builder.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${GENESIS_RECOMP_ROOT:-$REPO/segagenesisrecomp}"
APP_NAME="Sonic1"
CMAKE_TARGET="SonicTheHedgehogRecomp"
ROM_EXTS="md bin gen"
EXTRA_ARGS=""
REGEN_CMD=""
PREBUILD_CMD=""
POSTBUILD_CMD=""
PROD_CMAKE_FLAGS=( -DGEN_ENABLE_TRACE=OFF -DGEN_DEV_TRACE=OFF -DSONIC_REVERSE_DEBUG=OFF )
DEBUG_CMAKE_FLAGS=( -DGEN_ENABLE_TRACE=ON -DGEN_DEV_TRACE=ON -DSONIC_REVERSE_DEBUG=OFF )
BUNDLE_ID="com.mstan.sonic1recomp"
source "$ENGINE/tools/release/build-macos.sh" "$@"

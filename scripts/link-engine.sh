#!/usr/bin/env bash
# Link the shared segagenesisrecomp engine checkout into this game repo (macOS/Linux).
#
# The recompiler lives in ONE canonical checkout at the workspace root
# (../segagenesisrecomp), shared by all game projects (Sonic 1/2/3/...).
# This repo references it through a gitignored symlink, so no project "owns"
# the engine. The Windows equivalent is scripts/link-engine.bat (mklink /J).
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
target="../segagenesisrecomp"

if [ ! -d "$target" ]; then
    echo "error: $repo_root/$target not found." >&2
    echo "Clone the shared engine checkout first:" >&2
    echo "  git clone --recursive https://github.com/mstan/segagenesisrecomp.git \"$repo_root/$target\"" >&2
    exit 1
fi

if [ -L segagenesisrecomp ]; then
    echo "segagenesisrecomp already linked -> $(readlink segagenesisrecomp)"
    exit 0
fi
if [ -e segagenesisrecomp ]; then
    echo "error: 'segagenesisrecomp' exists and is not a symlink; remove it first." >&2
    exit 1
fi

ln -s "$target" segagenesisrecomp
echo "linked segagenesisrecomp -> $target"

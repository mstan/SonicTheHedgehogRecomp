#!/usr/bin/env bash
# Local-dev engine override (macOS/Linux): share ONE engine checkout across
# every game repo instead of a per-repo submodule checkout.
#
# The public layout is the committed `segagenesisrecomp` submodule, so a plain
# `git clone --recursive` is self-contained and needs none of this. For local
# development across Sonic 1/2/3/..., clone the engine ONCE at the workspace
# root (../segagenesisrecomp) and run this to drop a gitignored `engine-local`
# symlink pointing at it. CMake prefers engine-local when present, otherwise it
# falls back to the submodule. The Windows equivalent is scripts/link-engine.bat.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
target="../segagenesisrecomp"

if [ ! -d "$target" ]; then
    echo "error: $repo_root/$target not found." >&2
    echo "Clone the shared engine checkout once at the workspace root:" >&2
    echo "  git clone --recursive https://github.com/mstan/segagenesisrecomp.git \"$repo_root/$target\"" >&2
    exit 1
fi

if [ -L engine-local ]; then
    echo "engine-local already linked -> $(readlink engine-local)"
    exit 0
fi
if [ -e engine-local ]; then
    echo "error: 'engine-local' exists and is not a symlink; remove it first." >&2
    exit 1
fi

ln -s "$target" engine-local
echo "linked engine-local -> $target  (CMake will use it instead of the submodule)"

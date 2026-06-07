@echo off
REM Local-dev engine override (Windows): share ONE engine checkout across every
REM game repo instead of a per-repo submodule checkout.
REM
REM The public layout is the committed `segagenesisrecomp` submodule, so a plain
REM `git clone --recursive` is self-contained and needs none of this. For local
REM development across Sonic 1/2/3/..., clone the engine ONCE at the workspace
REM root (..\segagenesisrecomp) and run this to create a gitignored `engine-local`
REM directory junction pointing at it. CMake prefers engine-local when present,
REM otherwise it falls back to the submodule. macOS/Linux: scripts/link-engine.sh.
setlocal
cd /d "%~dp0.."

if not exist "..\segagenesisrecomp" (
    echo error: ..\segagenesisrecomp not found.
    echo Clone the shared engine checkout once at the workspace root:
    echo   git clone --recursive https://github.com/mstan/segagenesisrecomp.git ..\segagenesisrecomp
    exit /b 1
)

if exist "engine-local" (
    echo engine-local already present.
    exit /b 0
)

mklink /J engine-local ..\segagenesisrecomp
echo linked engine-local -^> ..\segagenesisrecomp  (CMake will use it instead of the submodule)

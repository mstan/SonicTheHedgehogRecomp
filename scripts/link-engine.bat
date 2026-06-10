@echo off
REM Link the shared segagenesisrecomp engine checkout into this game repo (Windows).
REM
REM The recompiler lives in ONE canonical checkout at the workspace root
REM (..\segagenesisrecomp), shared by all game projects (Sonic 1/2/3/...).
REM This repo references it through a gitignored directory junction, so no
REM project "owns" the engine. The macOS/Linux equivalent is
REM scripts/link-engine.sh (ln -s).
setlocal
cd /d "%~dp0.."

if not exist "..\segagenesisrecomp" (
    echo error: ..\segagenesisrecomp not found.
    echo Clone the shared engine checkout first:
    echo   git clone --recursive https://github.com/mstan/segagenesisrecomp.git ..\segagenesisrecomp
    exit /b 1
)

if exist "segagenesisrecomp" (
    echo segagenesisrecomp already present.
    exit /b 0
)

mklink /J segagenesisrecomp ..\segagenesisrecomp

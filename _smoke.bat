@echo off
REM Sonic 1 visual-regression smoke check.
REM Launches the runner with smoke_run_right_ghz.input + framebuffer
REM hashing every 60 wall frames, compares to the checked-in baseline.
REM
REM Run after `_build_native.bat` whenever you change shared runner code,
REM regen with a recompiler change, or land a fix. A divergence means
REM the visible behaviour changed -- either fix the regression, or
REM refresh the baseline (see commands below).
REM
REM Usage:
REM   _smoke.bat                       # assert vs baseline (exit 1 on diff)
REM   _smoke.bat --write-baseline      # capture current as new baseline
REM   _smoke.bat --keep-log            # save runner stderr to smoke.log
REM
REM Exit codes:
REM   0 -- match
REM   1 -- DIVERGENCE (something visibly changed)
REM   2 -- runner / environment error
REM   3 -- no baseline file present yet (use --write-baseline)
@setlocal
@set ZONE_SMOKE=%~dp0segagenesisrecomp\tools\zone_smoke.py
@set INPUT=%~dp0tools\smoke_run_right_ghz.input
python "%ZONE_SMOKE%" --game sonic1 --input "%INPUT%" --hash-frames 60 %*
@endlocal

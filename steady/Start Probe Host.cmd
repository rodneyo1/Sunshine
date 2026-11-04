@echo off
set "STEADY_PROBE_ENABLE=1"
echo Close any existing Sunshine instance before using this opt-in diagnostic launcher.
echo This enables authenticated clock replies; keyboard input remains legacy.
"%~dp0sunshine.exe"

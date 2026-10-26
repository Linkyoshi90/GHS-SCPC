@echo off
rem Double-click to play: runs tools\launch.ps1 (logs to recomp\live.err.log) without touching the PowerShell
rem execution policy. Extra arguments are passed on, e.g. -Scale 3.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launch.ps1" %*

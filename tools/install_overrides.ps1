# Copies only the hand-written fixes (overrides\*.cpp) into PS2Recomp's runner.
# Use this after editing an override; use install_generated.ps1 after a recompile.

$root   = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $root "PS2Recomp\ps2xRuntime\src\runner"
Copy-Item (Join-Path $root "overrides\*.cpp") $runner -Force -PassThru | ForEach-Object { "Installed $($_.Name)" }

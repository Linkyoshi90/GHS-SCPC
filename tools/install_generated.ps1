# Copies the recompiled GHS code into PS2Recomp's runner.
#   .cpp files -> PS2Recomp\ps2xRuntime\src\runner\   (replaces the placeholder register_functions.cpp)
#   .h files   -> PS2Recomp\ps2xRuntime\include\
# Run again after every recompile. Old generated .cpp files are removed first so
# functions that no longer exist don't linger in the build.
#
# To undo (restores the PS2Recomp placeholder):
#   cd PS2Recomp; git clean -fq ps2xRuntime/src/runner; git checkout -- ps2xRuntime/src/runner

$root    = Split-Path -Parent $PSScriptRoot
$output  = Join-Path $root "recomp\output"
$runtime = Join-Path $root "PS2Recomp\ps2xRuntime"
$runner  = Join-Path $runtime "src\runner"
$include = Join-Path $runtime "include"

if (-not (Test-Path (Join-Path $output "register_functions.cpp"))) {
    throw "No recompiled output in $output. Run ps2_recomp.exe ghs.toml first."
}

# Remove previously installed generated files (everything except main.cpp, if PS2Recomp ever adds one)
Get-ChildItem $runner -Filter *.cpp | Where-Object { $_.Name -ne "main.cpp" } | Remove-Item

robocopy $output $runner *.cpp /NFL /NDL /NJH /NJS /NP /MT:16 | Out-Null
Copy-Item (Join-Path $output "ps2_recompiled_functions.h"), (Join-Path $output "ps2_recompiled_stubs.h") $include -Force

# Our hand-written game fixes (overrides\*.cpp)
Copy-Item (Join-Path $root "overrides\*.cpp") $runner -Force

$count = (Get-ChildItem $runner -Filter *.cpp).Count
"Installed $count .cpp files into $runner (generated code + overrides)"
"Installed 2 headers into $include"

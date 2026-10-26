# One-time build on a fresh clone (the launcher runs it; it also works by hand once game\ holds the disc files):
# clones PS2Recomp at the pinned commit, applies patches\ps2recomp-local.patch, builds the recompiler, then
# recompiles game\SLES_519.33, installs the generated code (tools\install_generated.ps1) and builds the runner.
# Needs git, Visual Studio 2026 with "Desktop development with C++", and internet (CMake downloads raylib, imgui, FFmpeg).
# Native commands are checked through $LASTEXITCODE: with redirected output, PowerShell 5.1 turns their stderr into errors.
$root = Split-Path -Parent $PSScriptRoot
$ps2 = Join-Path $root 'PS2Recomp'
$build = Join-Path $ps2 'out\build'
$exe = Join-Path $build 'ps2xRuntime\Release\ps2EntryRunner.exe'

if (-not (Test-Path (Join-Path $root 'game\SLES_519.33'))) { throw 'game\SLES_519.33 is missing: unpack the disc first (launcher).' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is missing. Install it from https://git-scm.com/download/win and try again.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmake = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
}
if (-not $cmake) { throw 'Visual Studio with "Desktop development with C++" is missing. Install it from https://visualstudio.microsoft.com/ and try again.' }

if (-not (Test-Path (Join-Path $ps2 '.git'))) {
    'Downloading PS2Recomp ...'
    git clone -q -c core.autocrlf=false https://github.com/ran-j/PS2Recomp.git $ps2
    if (-not $LASTEXITCODE) { git -C $ps2 -c advice.detachedHead=false checkout -q 2c5fbb9 }
    if (-not $LASTEXITCODE) { git -C $ps2 apply --whitespace=nowarn (Join-Path $root 'patches\ps2recomp-local.patch') }
    if ($LASTEXITCODE) { Remove-Item -Recurse -Force $ps2; throw 'Downloading or patching PS2Recomp failed (see above).' }
}

if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    'Configuring the build (downloads raylib, imgui and FFmpeg) ...'
    & $cmake -S $ps2 -B $build
    if ($LASTEXITCODE) { throw 'CMake configure failed (see above).' }
}
'Building the recompiler ...'
& $cmake --build $build --config Release --target ps2_recomp --parallel
if ($LASTEXITCODE) { throw 'Building the recompiler failed (see above).' }

'Recompiling the game (log: recomp\recomp.log) ...'
Push-Location (Join-Path $root 'recomp')
& (Join-Path $build 'ps2xRecomp\Release\ps2_recomp.exe') ghs.toml *> recomp.log
$code = $LASTEXITCODE
Pop-Location
if ($code) { throw 'Recompiling failed: see recomp\recomp.log.' }
& (Join-Path $PSScriptRoot 'install_generated.ps1')
# The cheat menu's item names (F5) from the game's text, when Python 3 is installed (optional: without it the lists
# show numbers). Game text: game\ghs_items.txt stays local.
if (-not (Test-Path (Join-Path $root 'game\ghs_items.txt'))) {
    foreach ($py in 'python', 'py') {
        if (-not (Get-Command $py -ErrorAction SilentlyContinue)) { continue }
        & $py (Join-Path $PSScriptRoot 'ghs_item_names.py') 2>$null
        if (-not $LASTEXITCODE) { break }
    }
}
# Configure again so the project lists the ~35000 generated files: a re-glob during the build comes too late for that build.
& $cmake -S $ps2 -B $build | Out-Null
'Building the game (30-60 minutes; log: recomp\runner-build.log) ...'
& $cmake --build $build --config Release --target ps2EntryRunner --parallel *> (Join-Path $root 'recomp\runner-build.log')
if ($LASTEXITCODE -or -not (Test-Path $exe)) {
    Select-String -Path (Join-Path $root 'recomp\runner-build.log') -Pattern ': error|fatal error' | Select-Object -First 10 | ForEach-Object { $_.Line.Trim() }
    throw 'The game build failed: see recomp\runner-build.log.'
}
'Build finished.'

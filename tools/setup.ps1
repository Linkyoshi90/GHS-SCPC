# One-time build on a fresh clone (the launcher runs it; it also works by hand once game\ holds the disc files):
# clones PS2Recomp at the pinned commit, applies patches\ps2recomp-local.patch, builds the recompiler, then
# recompiles game\SLES_519.33, installs the generated code (tools\install_generated.ps1) and builds the runner.
# After an update it runs again and only rebuilds what changed: a new patch updates just the files that differ, and
# the game is recompiled only when the recompiler or its inputs are newer than the last recompile.
# Needs git, Visual Studio 2026 with "Desktop development with C++", and internet (CMake downloads raylib, imgui, FFmpeg).
# Native commands are checked through $LASTEXITCODE: with redirected output, PowerShell 5.1 turns their stderr into errors.
$root = Split-Path -Parent $PSScriptRoot
$ps2 = Join-Path $root 'PS2Recomp'
$build = Join-Path $ps2 'out\build'
$exe = Join-Path $build 'ps2xRuntime\Release\ps2EntryRunner.exe'
# PS2Recomp's default ON compiles log tracking into every game function: far slower, ~1 GB game\ps2_log.txt per session
$options = '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF'

if (-not (Test-Path (Join-Path $root 'game\SLES_519.33'))) { throw 'game\SLES_519.33 is missing: unpack the disc first (launcher).' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is missing. Install it from https://git-scm.com/download/win and try again.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmake = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
}
if (-not $cmake) { throw 'Visual Studio with "Desktop development with C++" is missing. Install it from https://visualstudio.microsoft.com/ and try again.' }

$patch = Join-Path $root 'patches\ps2recomp-local.patch'
$applied = Join-Path $root 'recomp\ps2recomp-applied.patch' # the patch PS2Recomp holds now
if (-not (Test-Path (Join-Path $ps2 '.git'))) {
    'Downloading PS2Recomp ...'
    git clone -q -c core.autocrlf=false https://github.com/ran-j/PS2Recomp.git $ps2
    if (-not $LASTEXITCODE) { git -C $ps2 -c advice.detachedHead=false checkout -q 2c5fbb9 }
    if (-not $LASTEXITCODE) { git -C $ps2 apply --whitespace=nowarn $patch }
    if ($LASTEXITCODE) { Remove-Item -Recurse -Force $ps2; throw 'Downloading or patching PS2Recomp failed (see above).' }
    Copy-Item $patch $applied
}
elseif (-not (Test-Path $applied) -or (Get-FileHash $patch).Hash -ne (Get-FileHash $applied).Hash) {
    # An updated patch: apply it to the pinned commit in a scratch worktree and copy over only the files that differ.
    # Re-applying in place would rewrite every patched header, and with them recompile all ~35000 game files.
    'Updating PS2Recomp to the new patch ...'
    $tree = Join-Path $env:TEMP 'ghs_ps2recomp_patched'
    Remove-Item -Recurse -Force $tree -ErrorAction SilentlyContinue
    git -C $ps2 worktree prune
    git -C $ps2 worktree add -q --detach $tree 2c5fbb9
    if (-not $LASTEXITCODE) { git -C $tree apply --whitespace=nowarn $patch }
    if ($LASTEXITCODE) { throw 'Applying the updated PS2Recomp patch failed (see above). Deleting the PS2Recomp folder rebuilds everything.' }
    $paths = foreach ($p in @($patch, $applied) | Where-Object { Test-Path $_ }) {
        Select-String -Path $p -Pattern '^diff --git a/(\S+) b/(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value; $_.Matches[0].Groups[2].Value }
    }
    foreach ($rel in $paths | Sort-Object -Unique) {
        $new = Join-Path $tree $rel
        $cur = Join-Path $ps2 $rel
        if (-not (Test-Path $new)) { if (Test-Path $cur) { Remove-Item -Force $cur; "  removed $rel" }; continue }
        if ((Test-Path $cur) -and (Get-FileHash $new).Hash -eq (Get-FileHash $cur).Hash) { continue }
        New-Item -ItemType Directory -Force (Split-Path $cur) | Out-Null
        Copy-Item -Force $new $cur
        "  updated $rel"
    }
    git -C $ps2 worktree remove --force $tree
    Copy-Item -Force $patch $applied
}

if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    'Configuring the build (downloads raylib, imgui and FFmpeg) ...'
    & $cmake -S $ps2 -B $build $options
    if ($LASTEXITCODE) { throw 'CMake configure failed (see above).' }
}
'Building the recompiler ...'
& $cmake --build $build --config Release --target ps2_recomp --parallel
if ($LASTEXITCODE) { throw 'Building the recompiler failed (see above).' }

# A recompile rewrites all ~35000 generated files (a full rebuild), so only when the recompiler or its inputs changed.
$recompiler = Join-Path $build 'ps2xRecomp\Release\ps2_recomp.exe'
$lastRecompile = Join-Path $root 'recomp\output\register_functions.cpp'
$inputs = @($recompiler) + (@('recomp\ghs.toml', 'recomp\ghs_functions_code.csv', 'game\SLES_519.33') | ForEach-Object { Join-Path $root $_ })
if ((Test-Path $lastRecompile) -and -not (Get-Item $inputs | Where-Object { $_.LastWriteTime -gt (Get-Item $lastRecompile).LastWriteTime })) {
    'The recompiled game code is up to date.'
    & (Join-Path $PSScriptRoot 'install_overrides.ps1') | Out-Null
}
else {
    'Recompiling the game (log: recomp\recomp.log) ...'
    Push-Location (Join-Path $root 'recomp')
    & $recompiler ghs.toml *> recomp.log
    $code = $LASTEXITCODE
    Pop-Location
    if ($code) { throw 'Recompiling failed: see recomp\recomp.log.' }
    & (Join-Path $PSScriptRoot 'install_generated.ps1')
}
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
& $cmake -S $ps2 -B $build $options | Out-Null
'Building the game (30-60 minutes; log: recomp\runner-build.log) ...'
& $cmake --build $build --config Release --target ps2EntryRunner --parallel *> (Join-Path $root 'recomp\runner-build.log')
if ($LASTEXITCODE -or -not (Test-Path $exe)) {
    Select-String -Path (Join-Path $root 'recomp\runner-build.log') -Pattern ': error|fatal error' | Select-Object -First 10 | ForEach-Object { $_.Line.Trim() }
    throw 'The game build failed: see recomp\runner-build.log.'
}
# The VU1 graphics programs in recomp\vu1_recipes.txt, built from game\SLES_519.33 and translated, so the first
# session already runs at full speed (otherwise they run on the slow interpreter until the next Play).
'Translating the graphics programs (a few minutes) ...'
& (Join-Path $PSScriptRoot 'add_vu1_images.ps1')
'Build finished.'

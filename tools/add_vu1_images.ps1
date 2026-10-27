# Translates the VU1 programs the game ran on the slow path: the runner saves each untranslated 16 KB code image as
# <hash>.bin in vu1_untranslated\ next to the game ([GHS-VU1NEW] in the log). This copies new ones into
# recomp\vu1images, regenerates every image's C++ (PS2Recomp\ps2xRuntime\src\lib\vu\generated) and rebuilds the runner.
# It also translates the images in recomp\vu1_recipes.txt (built by the runner from game\SLES_519.33) when generated\
# lacks one of them: tools\setup.ps1 runs it after the first build, so a fresh build starts at full speed.
#   powershell -ExecutionPolicy Bypass -File tools\add_vu1_images.ps1 [-From <dir>,...] [-NoBuild]
# Game code: the images and the generated files stay local (not in git).
param([string[]]$From = @(), [switch]$NoBuild)
$root = Split-Path -Parent $PSScriptRoot
$images = Join-Path $root 'recomp\vu1images'
$generated = Join-Path $root 'PS2Recomp\ps2xRuntime\src\lib\vu\generated'
$exe = Join-Path $root 'PS2Recomp\out\build\ps2xRuntime\Release\ps2EntryRunner.exe'
$cmake = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
if (-not $From) { $From = @((Join-Path $root 'game\vu1_untranslated'), (Join-Path $root 'recomp\game_bench\vu1_untranslated')) }

if (Get-Process ps2EntryRunner* -ErrorAction SilentlyContinue) { 'The game is running: close it first.'; return }
New-Item -ItemType Directory -Force $images | Out-Null   # fresh clone: no images yet
$new = @()
foreach ($dir in $From) {
    if (-not (Test-Path $dir)) { continue }
    foreach ($f in Get-ChildItem $dir -Filter '*.bin') {
        if ($f.Length -ne 16384 -or $f.BaseName -notmatch '^[0-9a-f]{16}$') { "skipped $($f.FullName) (not a 16 KB image)"; continue }
        $dest = Join-Path $images $f.Name
        if (-not (Test-Path $dest)) { Copy-Item $f.FullName $dest; $new += $f.BaseName }
    }
}
$recipes = Join-Path $root 'recomp\vu1_recipes.txt'
$recipeHashes = @(Get-Content $recipes | Where-Object { $_ -match '^[0-9a-f]{16} ' } | ForEach-Object { $_.Substring(0, 16) })
$missing = @($recipeHashes | Where-Object { -not (Test-Path (Join-Path $generated "vu1_$_.inc")) })
if (-not $new -and -not $missing) { 'No new VU1 images.'; return }
if ($new) { "new images: $($new -join ', ')" }
if ($missing) { "$($missing.Count) VU1 images from recomp\vu1_recipes.txt to translate" }

$gen = Join-Path $env:TEMP 'ghs_vu1gen'
Remove-Item -Recurse -Force $gen -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $gen | Out-Null
$env:GHS_VU1REGEN_FROM = $images
$env:GHS_VU1GEN = $gen
$env:GHS_VU1REGEN_RECIPES = $recipes
$env:GHS_MC_ROOT = Join-Path $root 'recomp\memcards_bench'
$log = Join-Path $root 'recomp\regen.err.log'
$p = Start-Process -FilePath $exe -ArgumentList "`"$root\game\SLES_519.33`"" -WorkingDirectory "$root\game" -WindowStyle Minimized `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $root 'recomp\regen.log') -PassThru
if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; 'regeneration timed out (see recomp\regen.err.log)'; return }
Remove-Item Env:GHS_VU1REGEN_FROM, Env:GHS_VU1GEN, Env:GHS_VU1REGEN_RECIPES
$want = @(@(Get-ChildItem $images -Filter '*.bin' | ForEach-Object BaseName) + $recipeHashes | Sort-Object -Unique).Count
$got = @(Get-ChildItem $gen -Filter 'vu1_*.inc' | Where-Object { $_.Name -ne 'vu1_programs.inc' }).Count
if ($got -lt $want) { "only $got of $want images regenerated (see recomp\regen.err.log); generated\ left unchanged"; return }
$backup = Join-Path $root ('recomp\vu1gen_backup_' + (Get-Date -Format 'yyyyMMdd_HHmm'))
if (Test-Path $generated) { Copy-Item $generated $backup -Recurse }
New-Item -ItemType Directory -Force $generated | Out-Null
Copy-Item "$gen\*.inc" $generated -Force
# ps2_vu1_core.cpp includes generated\ via __has_include: the build doesn't track a folder that didn't exist before
(Get-Item (Join-Path $root 'PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp')).LastWriteTime = Get-Date
"regenerated $got images (old files in $backup)"

if ($NoBuild) { return }
Push-Location (Join-Path $root 'PS2Recomp')
& $cmake --build out/build --config Release --target ps2EntryRunner --parallel *> (Join-Path $root 'recomp\runner-build.log')
$code = $LASTEXITCODE
Pop-Location
"build: exit=$code"
Select-String -Path (Join-Path $root 'recomp\runner-build.log') -Pattern ': error|fatal error' | Select-Object -First 10 | ForEach-Object { $_.Line.Trim() }

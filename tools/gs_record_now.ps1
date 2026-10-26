# Saves the next 40 GS frames (~1.5 s) of a game started with tools\launch.ps1 -Record to recomp\gsbench_live.bin
# (VRAM + GS commands: game data, keep local). Run it right as the thing to look at starts; the game pauses
# ~3 s afterwards. Each recording overwrites the previous one, so it is copied to gsbench_live_<time>.bin.
$root = Split-Path -Parent $PSScriptRoot
$trigger = Join-Path $root 'recomp\gsbench_trigger'
$out = Join-Path $root 'recomp\gsbench_live.bin'
$log = Join-Path $root 'recomp\live.err.log'
if (-not (Get-Process ps2EntryRunner -ErrorAction SilentlyContinue)) { throw 'The game is not running (start it with tools\launch.ps1 -Record).' }
$before = if (Test-Path $out) { (Get-Item $out).LastWriteTime } else { [datetime]::MinValue }
Set-Content $trigger '1'
'Recording 10 frames...'
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $out) -and (Get-Item $out).LastWriteTime -gt $before) { Start-Sleep 2; break }
}
Remove-Item $trigger -ErrorAction SilentlyContinue
if ((Test-Path $out) -and (Get-Item $out).LastWriteTime -gt $before) {
    $keep = Join-Path $root ('recomp\gsbench_live_{0:HHmmss}.bin' -f (Get-Date))
    Copy-Item $out $keep
    "Saved $keep ($([math]::Round((Get-Item $keep).Length / 1MB, 1)) MB). You can keep playing."
}
else { "Nothing saved yet: was the game started with -Record? (see $log)" }

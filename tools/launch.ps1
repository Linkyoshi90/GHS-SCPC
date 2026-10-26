# Starts the port for hand-playing (visible window), saves the output, keeps the memory cards out of game\.
#   -Capture     also records every VU1 program the game runs (recomp\vu1progs) and prints call counts every 5 s
#                ([GHS-VU1PROGS]); play the slow areas (hallway) for a minute, then tell Claude to regenerate.
#   -NoVu1Async / -NoGifAsync   run VU1 / GIF processing on the game thread (A/B for the broken geometry).
#   -Bands N     GS threads (default: one per physical core, hardware threads / 2, 4..12).
#   -TexCheck    check every decoded-texture cache hit against VRAM (texture glitch hunting; slower).
#   -Pcm         record all sound output to recomp\live.pcm (audio glitch hunting).
#   -Record      allow GS recordings: run tools\gs_record_now.ps1 while the glitch is on screen.
#   -Cpu         GS on the CPU (GHS_GS_GPU=0); by default it runs on the GPU (D3D11 compute, docs\GS_GPU_BACKEND.md)
#                at the render scale saved in F2 -> Resolution. -Scale N (1-4) overrides that scale (GHS_GS_SCALE=N).
#   -Online host:port -Room name   online two-player Hell's Roulette through tools\roulette_server.py (GHS_NET, GHS_ROOM);
#                the first in the room plays blue, the second red. Both pick Hell's Roulette, two players, on the title screen.
# Keys (defaults; change them in F2 -> Controls, F4): WASD run (hold C to walk, Left Ctrl to walk slowly), arrows = D-pad (slow walk, menus), X/Space cross, C/Esc circle, Z square, V triangle,
#       Q/E L1/R1, Left/Right Shift L2/R2, Enter start, Tab select. F1 debugger, F2 settings, F3 fps number, F4 key bindings,
#       F5 cheats, F11 full screen, Backspace (hold) fast-forward. Cheat item names: tools\setup.ps1 writes them (needs Python 3), or python tools\ghs_item_names.py.
# Output: recomp\live.log (stdout), recomp\live.err.log (the [GHS-*] lines).
param([switch]$Capture, [switch]$NoVu1Async, [switch]$NoGifAsync, [int]$Bands = 0, [switch]$TexCheck, [switch]$Pcm, [switch]$Record, [switch]$Cpu, [switch]$Gpu, [int]$Scale = 0, [string]$Online = '', [string]$Room = 'default')
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'PS2Recomp\out\build\ps2xRuntime\Release\ps2EntryRunner.exe'
if (-not (Test-Path $exe)) { throw "Runner not built: $exe" }
$env:GHS_MC_ROOT = Join-Path $root 'recomp\memcards'
foreach ($name in 'live.log', 'live.err.log') { # keep the previous run's logs (live.prev.*)
    $old = Join-Path $root "recomp\$name"
    if (Test-Path $old) { Move-Item -Force $old (Join-Path $root ("recomp\" + ($name -replace '^live', 'live.prev'))) }
}
$trace = Join-Path $root 'recomp\port_trace.log' # RAM watch trace (overrides), incl. the memory card UI state
if (Test-Path $trace) { Move-Item -Force $trace (Join-Path $root 'recomp\port_trace.prev.log') }
if ($Capture) { New-Item -ItemType Directory -Force (Join-Path $root 'recomp\vu1progs') | Out-Null; $env:GHS_VU1CAPTURE = '1' }
if ($NoVu1Async) { $env:GHS_VU1_ASYNC = '0' }
if ($NoGifAsync) { $env:GHS_GIF_ASYNC = '0' }
if ($Bands -gt 0) { $env:GHS_GS_BANDS = [string]$Bands }
# These two are cleared when not given (the script runs in the caller's session, its variables would stay).
# (-Gpu: the default, kept so older command lines work.)
if ($Cpu) { $env:GHS_GS_GPU = '0' } else { Remove-Item Env:GHS_GS_GPU -ErrorAction SilentlyContinue }
if ($Scale -gt 0) { $env:GHS_GS_SCALE = [string]$Scale } else { Remove-Item Env:GHS_GS_SCALE -ErrorAction SilentlyContinue }
if ($Online) { $env:GHS_NET = $Online; $env:GHS_ROOM = $Room } else { Remove-Item Env:GHS_NET -ErrorAction SilentlyContinue }
if ($TexCheck) { $env:GHS_TEXCACHE_CHECK = '1' } # check every cached texel against VRAM ([GHS-TEXCACHE] lines), slower
if ($Pcm) { $env:GHS_PCMFILE = Join-Path $root 'recomp\live.pcm' } # everything played, raw 48 kHz stereo s16 (~11 MB/min)
if ($Record) { # tools\gs_record_now.ps1 then saves the next 10 GS frames (game data: keep local)
    Remove-Item (Join-Path $root 'recomp\gsbench_trigger') -ErrorAction SilentlyContinue
    $env:GHS_GS_BENCH = Join-Path $root 'recomp\gsbench_trigger'
    $env:GHS_GS_BENCH_SAVE = Join-Path $root 'recomp\gsbench_live.bin'
    $env:GHS_GS_BENCH_FRAMES = '40' # ~1.5 s of game; recording again overwrites the file
    $env:GHS_GS_BENCH_PASSES = '1'  # one timing replay: the game pauses ~3 s after each recording
}
Start-Process -FilePath $exe -ArgumentList "`"$(Join-Path $root 'game\SLES_519.33')`"" -WorkingDirectory (Join-Path $root 'game') `
    -RedirectStandardOutput (Join-Path $root 'recomp\live.log') -RedirectStandardError (Join-Path $root 'recomp\live.err.log')
'Started. Close the game window when done (or: Stop-Process -Name ps2EntryRunner).'

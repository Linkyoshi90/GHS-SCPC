#!/usr/bin/env bash
# Linux version of tools/launch.ps1: starts the game (built by tools/setup.sh), saves the output, keeps the memory cards
# out of game/. Runs until the game window is closed.
#   --cpu          GS on the CPU (GHS_GS_GPU=0); by default it runs on the GPU through Vulkan when the build has it
#   --scale N      render scale 1-4 (GHS_GS_SCALE=N; default: F2 -> Resolution)
#   --bands N      GS threads (default: one per physical core)
#   --online host:port [--room name]   online two-player Hell's Roulette through tools/roulette_server.py
#   --capture      record every VU1 program the game runs (recomp/vu1progs), call counts every 5 s ([GHS-VU1PROGS])
#   --texcheck     check every decoded-texture cache hit against VRAM (slower)
#   --pcm          record all sound output to recomp/live.pcm (raw 48 kHz stereo s16)
# Keys: as in launch.ps1 (F2 settings, F4 key bindings, F1 debugger, F11 full screen).
# Output: recomp/live.log (stdout), recomp/live.err.log (the [GHS-*] lines); the previous run's as live.prev.*.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
exe=$root/PS2Recomp/out/linux/ps2xRuntime/ps2EntryRunner
[ -x "$exe" ] || { echo "Runner not built: $exe (run tools/setup.sh)" >&2; exit 1; }
# Cleared when not given: an exported value from an earlier run in this shell would otherwise stay.
unset GHS_GS_GPU GHS_GS_API GHS_GS_SCALE GHS_GS_BANDS GHS_NET GHS_ROOM GHS_VU1CAPTURE GHS_TEXCACHE_CHECK GHS_PCMFILE
room=default
while [ $# -gt 0 ]; do
    case $1 in
        --cpu) export GHS_GS_GPU=0 ;;
        --scale) export GHS_GS_SCALE=$2; shift ;;
        --bands) export GHS_GS_BANDS=$2; shift ;;
        --online) export GHS_NET=$2; shift ;;
        --room) room=$2; shift ;;
        --capture) mkdir -p "$root/recomp/vu1progs"; export GHS_VU1CAPTURE=1 ;;
        --texcheck) export GHS_TEXCACHE_CHECK=1 ;;
        --pcm) export GHS_PCMFILE=$root/recomp/live.pcm ;;
        *) echo "Unknown option: $1 (see the top of $0)" >&2; exit 1 ;;
    esac
    shift
done
if [ -n "${GHS_NET:-}" ]; then export GHS_ROOM=$room; fi
export GHS_MC_ROOT=$root/recomp/memcards
for pair in live.log:live.prev.log live.err.log:live.prev.err.log port_trace.log:port_trace.prev.log; do # keep the previous run's
    [ ! -f "$root/recomp/${pair%:*}" ] || mv -f "$root/recomp/${pair%:*}" "$root/recomp/${pair#*:}"
done
echo 'Running. Close the game window when done (logs: recomp/live.log, recomp/live.err.log).'
cd "$root/game"
exec "$exe" "$root/game/SLES_519.33" > "$root/recomp/live.log" 2> "$root/recomp/live.err.log"

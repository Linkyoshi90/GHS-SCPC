#!/usr/bin/env bash
# Linux version of tools/add_vu1_images.ps1: translates the VU1 programs the game ran on the slow path. The runner saves
# each untranslated 16 KB code image as <hash>.bin in vu1_untranslated/ next to the game ([GHS-VU1NEW] in the log); this
# copies new ones into recomp/vu1images, regenerates every image's C++ (PS2Recomp/ps2xRuntime/src/lib/vu/generated)
# and rebuilds the runner. It also translates the images in recomp/vu1_recipes.txt when generated/ lacks one of them:
# tools/setup.sh runs it after the first build, so a fresh build starts at full speed.
#   tools/add_vu1_images.sh [--no-build] [dir ...]
# Game code: the images and the generated files stay local (not in git).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
images=$root/recomp/vu1images
generated=$root/PS2Recomp/ps2xRuntime/src/lib/vu/generated
build=$root/PS2Recomp/out/linux
exe=$build/ps2xRuntime/ps2EntryRunner
noBuild=0
if [ "${1:-}" = --no-build ]; then noBuild=1; shift; fi
from=("$@")
[ ${#from[@]} -gt 0 ] || from=("$root/game/vu1_untranslated" "$root/recomp/game_bench/vu1_untranslated")

if pgrep -x ps2EntryRunner >/dev/null; then echo 'The game is running: close it first.'; exit 0; fi
mkdir -p "$images" # fresh clone: no images yet
new=()
for dir in "${from[@]}"; do
    [ -d "$dir" ] || continue
    for f in "$dir"/*.bin; do
        [ -e "$f" ] || continue
        name=$(basename "$f" .bin)
        if [ "$(stat -c %s "$f")" -ne 16384 ] || ! [[ $name =~ ^[0-9a-f]{16}$ ]]; then echo "skipped $f (not a 16 KB image)"; continue; fi
        if [ ! -e "$images/$name.bin" ]; then cp "$f" "$images/"; new+=("$name"); fi
    done
done
recipes=$root/recomp/vu1_recipes.txt
recipeHashes=$(grep -oE '^[0-9a-f]{16} ' "$recipes" | cut -c1-16 || true)
missing=0
for h in $recipeHashes; do [ -e "$generated/vu1_$h.cpp" ] || missing=$((missing + 1)); done
if [ ${#new[@]} -eq 0 ] && [ $missing -eq 0 ]; then echo 'No new VU1 images.'; exit 0; fi
[ ${#new[@]} -eq 0 ] || echo "new images: ${new[*]}"
[ $missing -eq 0 ] || echo "$missing VU1 images from recomp/vu1_recipes.txt to translate"

gen=$(mktemp -d)
log=$root/recomp/regen.err.log
code=0 # the files written decide below, as in add_vu1_images.ps1; only the timeout is fatal here
(cd "$root/game" && GHS_VU1REGEN_FROM=$images GHS_VU1GEN=$gen GHS_VU1REGEN_RECIPES=$recipes \
    GHS_MC_ROOT=$root/recomp/memcards_bench timeout 300 "$exe" "$root/game/SLES_519.33" \
    > "$root/recomp/regen.log" 2> "$log") || code=$?
if [ $code -eq 124 ]; then echo 'regeneration timed out (see recomp/regen.err.log)'; rm -rf "$gen"; exit 0; fi
want=$( { for f in "$images"/*.bin; do [ -e "$f" ] && basename "$f" .bin; done; echo "$recipeHashes"; } | grep -E '^[0-9a-f]{16}$' | sort -u | wc -l)
got=$(find "$gen" -maxdepth 1 -regextype egrep -regex '.*/vu1_[0-9a-f]{16}\.cpp' | wc -l) # not vu1_steps_<k>.cpp
if [ "$got" -lt "$want" ]; then echo "only $got of $want images regenerated (see recomp/regen.err.log); generated/ left unchanged"; exit 0; fi
backup=$root/recomp/vu1gen_backup_$(date +%Y%m%d_%H%M)
if [ -d "$generated" ]; then cp -r "$generated" "$backup"; fi
mkdir -p "$generated"
rm -f "$generated"/* # older layouts (vu1_<hash>.inc) would linger next to the new files
cp "$gen"/* "$generated/"
rm -rf "$gen"
# ps2_vu1_core.cpp includes generated/ via __has_include: the build doesn't track a folder that didn't exist before
touch "$root/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp"
echo "regenerated $got images (old files in $backup)"

[ $noBuild -eq 0 ] || exit 0
# Each image is its own .cpp: configure so the project lists them (a re-glob during the build comes too late for it).
cmake -S "$root/PS2Recomp" -B "$build" > /dev/null
code=0
cmake --build "$build" --target ps2EntryRunner --parallel > "$root/recomp/runner-build.log" 2>&1 || code=$?
echo "build: exit=$code"
grep -m 10 -E 'error:|fatal error' "$root/recomp/runner-build.log" || true

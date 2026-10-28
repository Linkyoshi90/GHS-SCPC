#!/usr/bin/env bash
# One-time build on Linux (tools/setup.ps1 is the Windows one): unpacks the disc image when given one, clones PS2Recomp
# at the pinned commit, applies patches/ps2recomp-local.patch, builds the recompiler with gcc/clang, recompiles
# game/SLES_519.33, installs the generated code and builds the runner (PS2Recomp/out/linux/ps2xRuntime/ps2EntryRunner).
# After an update it runs again and only rebuilds what changed, as setup.ps1 does.
#   tools/setup.sh [disc.iso]   the European disc image (SLES-51933), unpacked into game/ first
# Needs git, cmake, a C++20 compiler, pkg-config, the FFmpeg and X11/OpenGL development packages, and internet (CMake
# downloads raylib and imgui). The GS runs on the GPU (Vulkan) when the build finds the Vulkan headers and dxc (the
# Vulkan SDK, VULKAN_SDK set); otherwise on the CPU.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
ps2=$root/PS2Recomp
build=$ps2/out/linux # out/build is the Windows (Visual Studio) build
exe=$build/ps2xRuntime/ps2EntryRunner
# PS2Recomp's default ON compiles log tracking into every game function: far slower, ~1 GB game/ps2_log.txt per session
options=(-DCMAKE_BUILD_TYPE=Release -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF)
die() { echo "$*" >&2; exit 1; }

if [ $# -ge 1 ]; then
    command -v bsdtar >/dev/null || die 'bsdtar is missing (Debian/Ubuntu: sudo apt install libarchive-tools).'
    [ -f "$1" ] || die "No such disc image: $1"
    echo "Unpacking $1 ..."
    mkdir -p "$root/game"
    bsdtar -xf "$1" -C "$root/game"
    sum=$(sha1sum "$root/game/SLES_519.33" 2>/dev/null | cut -d' ' -f1) || true
    [ "$sum" = 8676819aa0edc9f1b5efcdd1c0c97934da5b6528 ] ||
        die 'This is not the European disc v1.00 (SLES_519.33 missing or different): check the image against redump.org.'
fi
[ -f "$root/game/SLES_519.33" ] || die 'game/SLES_519.33 is missing: run tools/setup.sh <disc.iso> with the European disc image.'

missing=()
for tool in git cmake pkg-config c++; do command -v $tool >/dev/null || missing+=("$tool"); done
for lib in libavcodec libavformat libavutil libswresample libswscale x11 xrandr xinerama xcursor xi gl; do
    pkg-config --exists $lib 2>/dev/null || missing+=("$lib")
done
if [ ${#missing[@]} -gt 0 ]; then
    die "Missing: ${missing[*]}. Debian/Ubuntu: sudo apt install git cmake ninja-build g++ pkg-config libavcodec-dev \
libavformat-dev libavutil-dev libswresample-dev libswscale-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
libxi-dev libgl1-mesa-dev"
fi

patch=$root/patches/ps2recomp-local.patch
applied=$root/recomp/ps2recomp-applied.patch # the patch PS2Recomp holds now
if [ ! -d "$ps2/.git" ]; then
    echo 'Downloading PS2Recomp ...'
    if ! { git clone -q -c core.autocrlf=false https://github.com/ran-j/PS2Recomp.git "$ps2" &&
           git -C "$ps2" -c advice.detachedHead=false checkout -q 2c5fbb9 &&
           git -C "$ps2" apply --whitespace=nowarn "$patch"; }; then
        rm -rf "$ps2"
        die 'Downloading or patching PS2Recomp failed (see above).'
    fi
    cp "$patch" "$applied"
elif ! cmp -s "$patch" "$applied"; then
    # An updated patch: apply it to the pinned commit in a scratch worktree and copy over only the files that differ.
    # Re-applying in place would rewrite every patched header, and with them recompile all ~35000 game files.
    echo 'Updating PS2Recomp to the new patch ...'
    tree=${TMPDIR:-/tmp}/ghs_ps2recomp_patched
    rm -rf "$tree"
    git -C "$ps2" worktree prune
    git -C "$ps2" worktree add -q --detach "$tree" 2c5fbb9 && git -C "$tree" apply --whitespace=nowarn "$patch" ||
        die 'Applying the updated PS2Recomp patch failed (see above). Deleting the PS2Recomp folder rebuilds everything.'
    grep -h '^diff --git ' "$patch" "$applied" 2>/dev/null | awk '{print substr($3, 3); print substr($4, 3)}' | sort -u |
    while read -r rel; do
        if [ ! -e "$tree/$rel" ]; then
            if [ -e "$ps2/$rel" ]; then rm -f "$ps2/$rel"; echo "  removed $rel"; fi
            continue
        fi
        cmp -s "$tree/$rel" "$ps2/$rel" && continue
        mkdir -p "$(dirname "$ps2/$rel")"
        cp -f "$tree/$rel" "$ps2/$rel"
        echo "  updated $rel"
    done
    git -C "$ps2" worktree remove --force "$tree"
    cp -f "$patch" "$applied"
fi

if [ ! -f "$build/CMakeCache.txt" ]; then
    echo 'Configuring the build (downloads raylib and imgui) ...'
    if command -v ninja >/dev/null; then export CMAKE_GENERATOR=Ninja; fi
    cmake -S "$ps2" -B "$build" "${options[@]}"
fi
echo 'Building the recompiler ...'
cmake --build "$build" --target ps2_recomp --parallel

# Copies the recompiled game code into PS2Recomp's runner (tools/install_generated.ps1): .cpp files into src/runner
# (the old ones removed first, so functions that no longer exist don't linger), the headers into include/, then the
# hand-written fixes (overrides/*.cpp).
runner=$ps2/ps2xRuntime/src/runner
install_overrides() { cp -f "$root"/overrides/*.cpp "$runner/"; }
install_generated() {
    find "$runner" -maxdepth 1 -name '*.cpp' ! -name main.cpp -delete
    find "$root/recomp/output" -maxdepth 1 -name '*.cpp' -exec cp -t "$runner/" {} +
    cp -f "$root/recomp/output/ps2_recompiled_functions.h" "$root/recomp/output/ps2_recompiled_stubs.h" "$ps2/ps2xRuntime/include/"
    install_overrides
}

# A recompile rewrites all ~35000 generated files (a full rebuild), so only when the recompiler or its inputs changed.
recompiler=$build/ps2xRecomp/ps2_recomp
last=$root/recomp/output/register_functions.cpp
if [ -f "$last" ] && [ -z "$(find "$recompiler" "$root/recomp/ghs.toml" "$root/recomp/ghs_functions_code.csv" \
                                  "$root/game/SLES_519.33" -newer "$last")" ]; then
    echo 'The recompiled game code is up to date.'
    install_overrides
else
    echo 'Recompiling the game (log: recomp/recomp.log) ...'
    (cd "$root/recomp" && "$recompiler" ghs.toml > recomp.log 2>&1) || die 'Recompiling failed: see recomp/recomp.log.'
    install_generated
fi
# The cheat menu's item names (F5) from the game's text (optional: without Python 3 the lists show numbers).
if [ ! -f "$root/game/ghs_items.txt" ] && command -v python3 >/dev/null; then
    python3 "$root/tools/ghs_item_names.py" 2>/dev/null || true
fi
# Configure again so the project lists the ~35000 generated files: a re-glob during the build comes too late for that build.
cmake -S "$ps2" -B "$build" > /dev/null
echo 'Building the game (30-60 minutes; log: recomp/runner-build.log) ...'
if ! cmake --build "$build" --target ps2EntryRunner --parallel > "$root/recomp/runner-build.log" 2>&1 || [ ! -x "$exe" ]; then
    grep -m 10 -E 'error:|fatal error' "$root/recomp/runner-build.log" || true
    die 'The game build failed: see recomp/runner-build.log.'
fi
# The VU1 graphics programs in recomp/vu1_recipes.txt, built from game/SLES_519.33 and translated, so the first
# session already runs at full speed (otherwise they run on the slow interpreter until the next Play).
echo 'Translating the graphics programs (a few minutes) ...'
"$root/tools/add_vu1_images.sh"
echo 'Build finished. Start the game with tools/launch.sh'

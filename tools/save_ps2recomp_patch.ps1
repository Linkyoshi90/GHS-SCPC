# Saves every PS2Recomp change (commits and uncommitted work) against the pinned upstream commit to
# patches\ps2recomp-local.patch, which tools\setup.ps1 applies to a fresh clone. Run it after changing PS2Recomp.
# The generated game code (src/runner, vu/generated) stays out.
$root = Split-Path -Parent $PSScriptRoot
$ps2 = Join-Path $root 'PS2Recomp'
git -C $ps2 add -N .   # new files show up in the diff (intent-to-add only, nothing is staged)
git -C $ps2 diff --binary --output="$root\patches\ps2recomp-local.patch" 2c5fbb9 -- . ':!ps2xRuntime/src/runner' ':!ps2xRuntime/src/lib/vu/generated'
"Saved patches\ps2recomp-local.patch"

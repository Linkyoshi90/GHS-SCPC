"""Mods: builds game\\FILE.STM from the original and every mod in mods\\.
  python tools\\ghsmod.py                           build (keeps the original as game\\FILE.STM.orig; no mods: original)
  python tools\\ghsmod.py export <entry> <mod>       textures of an entry as PNG into mods\\<mod>\\FILE.STM\\
  python tools\\ghsmod.py restart <pid> <program> [arguments]   the game's Mods tab (F2): waits until the game (pid)
                                                    has closed, builds, starts it again; output in recomp\\mods.log

A mod is a folder or a .zip in mods\\ (its files at the top, or all in one folder), applied in name order (a later
mod wins where two change the same thing); the ones named in mods\\disabled.txt (one per line) are left out.
game\\FILE.STM.mods lists the mods built in (name, files, bytes), for the game's Mods tab. Inside a mod:
  FILE.STM\\<entry>              replaces that entry (names as tools\\ghsarc.py unpack writes them, e.g. 044.slid.bin)
  FILE.STM\\<entry>.tex<N>.png   texture N of that entry, same size as exported; colours that are not in its palette
                                get a new one (16 or 256 colours, tools\\ghstex.py)
  *.txt                         text replacement lists (tools\\ghs_textpatch.py); one without tabs is a readme
Other files (readme, notes) are skipped.
"""
import os
import re
import shutil
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghs_textpatch
import ghsarc
import ghstex

MODS = os.path.join(ghsarc.ROOT, "mods")
ORIG = ghsarc.GAME_FILE_STM + ".orig"
BUILT = ghsarc.GAME_FILE_STM + ".mods"


def enabled_mods():
    """[(name, files, bytes)] of the mods to build, in order; files/bytes tell the game's Mods tab when one changed."""
    if not os.path.isdir(MODS):
        return []
    try:
        with open(os.path.join(MODS, "disabled.txt"), encoding="utf-8") as f:
            disabled = {line.strip() for line in f}
    except FileNotFoundError:
        disabled = set()
    out = []
    for n in sorted(os.listdir(MODS)):
        path = os.path.join(MODS, n)
        if n in disabled or not (os.path.isdir(path) or n.lower().endswith(".zip")):
            continue
        sizes = [os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs] if os.path.isdir(path) else [os.path.getsize(path)]
        out.append((n, len(sizes), sum(sizes)))
    return out


def mod_files(path):
    """{name with / separators: bytes} of a mod folder or zip."""
    if os.path.isdir(path):
        out = {}
        for d, _, files in os.walk(path):
            for f in files:
                with open(os.path.join(d, f), "rb") as h:
                    out[os.path.relpath(os.path.join(d, f), path).replace(os.sep, "/")] = h.read()
        return out
    with zipfile.ZipFile(path) as z:
        out = {n: z.read(n) for n in z.namelist() if not n.endswith("/")}
    tops = {n.split("/")[0] for n in out}
    if len(tops) == 1 and all("/" in n for n in out) and tops != {"FILE.STM"}:
        out = {n.split("/", 1)[1]: b for n, b in out.items()}  # zipped with its folder
    return out


def texture_op(k, blob, where):
    def op(data):
        imgs = ghstex.images(data)
        if not imgs or k >= len(imgs):
            raise ValueError("%s: the entry has no texture %d" % (where, k))
        w, h, rgba = ghstex.read_png(blob)
        if (w, h) != imgs[k][5:7]:
            raise ValueError("%s: %dx%d, the game's texture is %dx%d" % (where, w, h, imgs[k][5], imgs[k][6]))
        return data if ghstex.decode(data, imgs[k]) == rgba else ghstex.encode(data, imgs[k], rgba)
    return op


def game_running():
    """Whether a game runs: it reads FILE.STM by offsets it loaded at start, so the file must not change under it."""
    if os.name != "nt":
        return False  # ponytail: the Linux build has no check yet; close the game before building
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ps2EntryRunner.exe", "/NH"], capture_output=True, text=True).stdout
    return "ps2EntryRunner.exe" in out


def build():
    if game_running():
        raise SystemExit("the game is running: close it first (or use F2 -> Mods -> Restart in the game)\nnothing written")
    if not os.path.exists(ORIG):
        shutil.copyfile(ghsarc.GAME_FILE_STM, ORIG)
    with open(ORIG, "rb") as f:
        data = f.read()
    ops = {}  # entry path -> [(where, function of the entry's bytes)], in mod order
    mods = enabled_mods()
    names = [m[0] for m in mods]
    for mod in names:
        try:
            files = mod_files(os.path.join(MODS, mod))
        except zipfile.BadZipFile:
            raise SystemExit("mods/%s: not a zip file\nnothing written" % mod)
        for name, blob in sorted(files.items()):
            where = "mods/%s/%s" % (mod, name)
            tex = re.fullmatch(r"FILE\.STM/(.+)\.tex(\d+)\.png", name, re.I)
            if tex:
                ops.setdefault(tex.group(1), []).append((where, texture_op(int(tex.group(2)), blob, where)))
            elif name.upper().startswith("FILE.STM/"):
                ops.setdefault(name[9:], []).append((where, lambda d, b=blob: b))
            elif name.lower().endswith(".txt") and "/" not in name and b"\t" in blob:  # without tabs: a readme
                try:
                    lists = ghs_textpatch.parse(blob.decode("utf-8"), where)
                except (ValueError, UnicodeDecodeError) as e:
                    raise SystemExit("%s: %s\nnothing written" % (where, e))
                for entry, reps in lists.items():
                    ops.setdefault(entry, []).append((where, lambda d, r=reps: ghs_textpatch.apply(d, r)))
            else:
                print("skipped", where)
        print("mod", mod)
    changed = []
    def runner(fns):
        def run(d):
            old = d
            for where, fn in fns:
                try:
                    d = fn(d)
                except ValueError as e:
                    raise SystemExit("%s\nnothing written" % (e if str(e).startswith(where) else "%s: %s" % (where, e)))
            if d != old:
                changed.append(fns)
            return d
        return run
    try:
        data = ghsarc.edit(data, {tuple(entry.split("/")): runner(fns) for entry, fns in ops.items()})
    except KeyError as e:
        bad = e.args[0]
        where = next(fns[0][0] for entry, fns in ops.items() if entry == bad or entry.startswith(bad + "/"))
        raise SystemExit("%s: FILE.STM has no entry %s\nnothing written" % (where, bad))
    changed = len(changed)
    with open(ghsarc.GAME_FILE_STM + ".tmp", "wb") as f:
        f.write(data)
    try:
        os.replace(ghsarc.GAME_FILE_STM + ".tmp", ghsarc.GAME_FILE_STM)
    except PermissionError:
        os.remove(ghsarc.GAME_FILE_STM + ".tmp")
        raise SystemExit("game\\FILE.STM is in use: close the game first (or use F2 -> Mods -> Restart in the game)\nnothing written")
    with open(BUILT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines("%s\t%d\t%d\n" % m for m in mods)
    print("built", ghsarc.GAME_FILE_STM, "from", len(names), "mods,", changed, "entries changed")


def restart(pid, program, args):
    """For the game's Mods tab: wait for the game to close (20 s, then end it), build, start it again in the same
    folder with the same environment and logs as the launcher. A failed build keeps the last FILE.STM."""
    recomp = os.path.join(ghsarc.ROOT, "recomp")
    os.makedirs(recomp, exist_ok=True)
    sys.stdout = sys.stderr = open(os.path.join(recomp, "mods.log"), "w", encoding="utf-8", buffering=1)
    print("waiting for the game (pid %d) to close" % pid)  # the game closes once this file exists
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x00100001, False, pid)  # SYNCHRONIZE | PROCESS_TERMINATE
        if h:
            if k32.WaitForSingleObject(h, 20000) != 0:
                print("the game did not close in 20 s: ending it")
                k32.TerminateProcess(h, 1)
                k32.WaitForSingleObject(h, 5000)
            k32.CloseHandle(h)
    else:
        import signal, time
        for _ in range(200):
            try:
                os.kill(pid, 0)
            except OSError:
                break
            time.sleep(0.1)
        else:
            os.kill(pid, signal.SIGKILL)
    try:
        build()
    except SystemExit as e:
        print("build failed:", e)
    except Exception:
        import traceback
        traceback.print_exc()
    for name, prev in (("live.log", "live.prev.log"), ("live.err.log", "live.prev.err.log"), ("port_trace.log", "port_trace.prev.log")):
        try:
            os.replace(os.path.join(recomp, name), os.path.join(recomp, prev))
        except OSError:
            pass
    import subprocess
    log, err = os.path.join(recomp, "live.log"), os.path.join(recomp, "live.err.log")
    if os.name == "nt":  # cmd writes the logs, as in the launcher
        cmd = 'cmd /c ""%s" %s >"%s" 2>"%s""' % (program, " ".join('"%s"' % a for a in args), log, err)
        subprocess.Popen(cmd, cwd=os.getcwd(), creationflags=0x08000000, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # CREATE_NO_WINDOW
    else:
        subprocess.Popen([program] + args, stdout=open(log, "w"), stderr=open(err, "w"), start_new_session=True)
    print("started", program, *args)


def export(entry, mod):
    with open(ORIG if os.path.exists(ORIG) else ghsarc.GAME_FILE_STM, "rb") as f:
        data = ghsarc.get(f.read(), tuple(entry.replace("\\", "/").split("/")))
    imgs = ghstex.images(data)
    if not imgs:
        raise SystemExit(entry + " is not a texture entry")
    dst = os.path.join(MODS, mod, "FILE.STM", os.path.dirname(entry))
    os.makedirs(dst, exist_ok=True)
    for k, img in enumerate(imgs):
        path = os.path.join(dst, "%s.tex%d.png" % (os.path.basename(entry), k))
        if os.path.exists(path):
            raise SystemExit(path + " exists (it may hold your changes)")
        ghstex.write_png(path, img[5], img[6], ghstex.decode(data, img))
        print("wrote", path, "%dx%d, %d colours" % (img[5], img[6], 16 if img[2] == 8 else 256))


if __name__ == "__main__":
    if sys.argv[1:] == []:
        build()
    elif len(sys.argv) == 4 and sys.argv[1] == "export":
        export(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == "restart" and sys.argv[2].isdigit():
        restart(int(sys.argv[2]), sys.argv[3], sys.argv[4:])
    else:
        raise SystemExit(__doc__)

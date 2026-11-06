"""Builds a release of CSNZ Offline Revived into dist/ (at the repository's root):

  dist/CSNZ-Offline-Revived-<version>.zip      the mod: every file Git tracks under mod/ at HEAD, the built binaries
                                               (botassist.asi, CSNZ_Server.exe and its DLLs, nartool.exe) and
                                               mod/patches.json (the client patches with their file offsets)
  dist/CSNZ-Offline-Revived-Launcher.exe       the launcher (launcher/, .NET Framework 4.8)

No file of the game goes in: the launcher patches the player's own Steam copy. Nor does anything of the player's:
accounts (UserDatabase.db3), the server's certificate and key (the launcher makes them on each PC), logs, backups,
or the pictures exported from the game (the launcher's More > Export... makes them from the player's game).

Usage: python mod/tools/release/build_release.py      (version: mod/VERSION; needs git, pefile and the .NET SDK)
"""
import io
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MOD = ROOT / "mod"
DIST = ROOT / "dist"
sys.path.insert(0, str(MOD / "tools"))
import apply_patches  # noqa: E402

# built files Git doesn't keep (paths under mod/)
BINARIES = [
    "tools/botassist/botassist.asi",
    "server/CSNZ_Server/bin/CSNZ_Server.exe",
    "server/CSNZ_Server/bin/libgcc_s_seh-1.dll",
    "server/CSNZ_Server/bin/libstdc++-6.dll",
    "server/CSNZ_Server/bin/libwinpthread-1.dll",
    "tools/nartool/nartool.exe",
]
# tracked but not for players
LEAVE_OUT = ["mod/CSNZ_Offline.ps1"]  # the developer's old launcher (his paths)


def patch_list(version: str) -> dict:
    """apply_patches.PATCHES with file offsets, read from the untouched copies (or the game, whose layout is the same)."""
    out = {"version": version, "patches": [], "files": []}
    for name, va, orig, new, note in apply_patches.PATCHES:
        src = MOD / "backup" / "original" / "Bin" / name
        if not src.exists():
            sys.exit(f"missing {src}: the offsets come from the original files")
        out["patches"].append({"file": name, "va": hex(va), "offset": apply_patches.file_offset(src, va),
                               "orig": orig.hex(), "new": new.hex(), "note": note})
    for src, name, overwrite in apply_patches.EXTRA_FILES:
        out["files"].append({"src": src.relative_to(MOD).as_posix(), "dst": name, "overwrite": overwrite})
    return out


def main():
    version = (MOD / "VERSION").read_text().strip()
    DIST.mkdir(exist_ok=True)
    for b in BINARIES:
        if not (MOD / b).exists():
            sys.exit(f"missing build output mod/{b}")

    # the mod at HEAD, as Git has it
    tar = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", "HEAD", "mod"], check=True, capture_output=True).stdout
    zpath = DIST / f"CSNZ-Offline-Revived-{version}.zip"
    count = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z, tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not m.isfile() or m.name in LEAVE_OUT:
                continue
            z.writestr(m.name, t.extractfile(m).read())
            count += 1
        for b in BINARIES:
            z.write(MOD / b, "mod/" + b)
            count += 1
        z.writestr("mod/patches.json", json.dumps(patch_list(version), indent=1))
        z.writestr("mod/VERSION", version + "\n")
    print(f"{zpath.name}: {count + 2} files, {zpath.stat().st_size / 1048576:.1f} MB")

    # the launcher, with the same version
    out = DIST / "launcher-build"
    subprocess.run(["dotnet", "build", str(ROOT / "launcher" / "CsnzLauncher.csproj"), "-c", "Release", "-o", str(out),
                    f"-p:Version={version}", "-nologo", "-v", "q"], check=True)
    exe = DIST / "CSNZ-Offline-Revived-Launcher.exe"
    shutil.copy2(out / "CSNZ-Offline-Revived-Launcher.exe", exe)
    shutil.rmtree(out)
    print(f"{exe.name}: {exe.stat().st_size / 1024:.0f} KB")


if __name__ == "__main__":
    main()

"""Builds a release of CSNZ Offline Revived into dist/ (at the repository's root):

  dist/CSNZ-Offline-Revived-<version>.zip      the mod: every file Git tracks under mod/ at HEAD, the built binaries
                                               (botassist.asi, CSNZ_Server.exe and its DLLs, nartool.exe) and
                                               mod/patches.json (the client patches with their file offsets)
  dist/CSNZ-Offline-Revived-Launcher.exe       the launcher (launcher/, .NET Framework 4.8)

No file of the game goes in: the launcher patches the player's own Steam copy. Nor does anything of the player's:
accounts (UserDatabase.db3), the server's certificate and key (the launcher makes them on each PC), logs, backups,
or the pictures exported from the game (the launcher's Pictures button makes them from the player's game).

Usage: python mod/tools/release/build_release.py      (version: mod/VERSION; needs git, pefile and the .NET SDK)
       Then it installs the release into Play/ (the user's own copy) for a local test before publishing, like the
       launcher's Update: accounts, settings and state stay. --no-play skips that; --play-only only does that.

Signing: with CSNZ_SIGN_CERT set to the SHA-1 thumbprint of a code signing certificate in the Windows certificate
store (a Certum open source certificate through SimplySign, for example), our own programs are signed with signtool
before they're packed: the launcher, botassist.asi, CSNZ_Server.exe, nartool.exe and the Steam bridge. The MinGW
DLLs aren't ours and stay as they come. CSNZ_SIGN_TIMESTAMP changes the timestamp server (Certum's by default).
Antivirus programs and Windows SmartScreen trust signed programs far more than unsigned ones.
"""
import io
import json
import os
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
    "tools/steambridge/out/CSNZ-SteamBridge.exe",  # built below (online play through Steam)
    "tools/steambridge/out/CSNZ-SteamBridge.exe.config",
]
# our own programs, signed when CSNZ_SIGN_CERT is set
SIGNED = [
    "tools/botassist/botassist.asi",
    "server/CSNZ_Server/bin/CSNZ_Server.exe",
    "tools/nartool/nartool.exe",
    "tools/steambridge/out/CSNZ-SteamBridge.exe",
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
                               "orig": orig.hex(), "new": new.hex(), "note": note,
                               "prev": [b.hex() for b in apply_patches.PREVIOUS.get((name, va), [])]})
    for src, name, overwrite in apply_patches.EXTRA_FILES:
        out["files"].append({"src": src.relative_to(MOD).as_posix(), "dst": name, "overwrite": overwrite})
    return out


def signtool():
    kits = sorted(Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "bin").glob("10.*/x64/signtool.exe"))
    if not kits:
        sys.exit("CSNZ_SIGN_CERT is set but signtool.exe wasn't found (Windows SDK)")
    return str(kits[-1])


def sign(files):
    """Signs the files with the certificate named by CSNZ_SIGN_CERT (nothing happens without it)."""
    cert = os.environ.get("CSNZ_SIGN_CERT", "").replace(" ", "")
    if not cert:
        print("not signed (CSNZ_SIGN_CERT isn't set)")
        return
    stamp = os.environ.get("CSNZ_SIGN_TIMESTAMP", "http://time.certum.pl")
    subprocess.run([signtool(), "sign", "/sha1", cert, "/fd", "sha256", "/tr", stamp, "/td", "sha256",
                    "/d", "Counter-Strike Nexon: Offline Revived", "/du", "https://github.com/ElSerbe/Counter-Strike-Nexon-Offline-Revived",
                    *[str(f) for f in files]], check=True)
    subprocess.run([signtool(), "verify", "/pa", *[str(f) for f in files]], check=True, stdout=subprocess.DEVNULL)
    print(f"signed {len(files)} files")


def main():
    version = (MOD / "VERSION").read_text().strip()
    DIST.mkdir(exist_ok=True)
    # the Steam bridge (x86, .NET Framework 4.8)
    subprocess.run(["dotnet", "build", str(MOD / "tools" / "steambridge" / "SteamBridge.csproj"), "-c", "Release",
                    "-o", str(MOD / "tools" / "steambridge" / "out"), f"-p:Version={version}", "-nologo", "-v", "q"], check=True)
    for b in BINARIES:
        if not (MOD / b).exists():
            sys.exit(f"missing build output mod/{b}")
    sign([MOD / b for b in SIGNED])

    # the mod at HEAD, as Git has it
    tar = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", "HEAD", "mod"], check=True, capture_output=True).stdout
    zpath = DIST / f"CSNZ-Offline-Revived-{version}.zip"
    count = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z, tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not m.isfile() or m.name in LEAVE_OUT or m.name == "mod/VERSION":
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
    sign([exe])
    shutil.rmtree(out)
    print(f"{exe.name}: {exe.stat().st_size / 1024:.0f} KB")
    if "--no-play" not in sys.argv:
        install_into_play(zpath, exe)


# the player's state: kept when an update brings its own copy (the launcher's Installer.KeepIfPresent)
KEEP_IF_PRESENT = {
    "mod/server/CSNZ_Server/bin/Data/BotPresets.json",
    "mod/server/CSNZ_Server/bin/Data/BingoState.json",
    "mod/server/CSNZ_Server/bin/Data/ZSRiftState.json",
}


def install_into_play(zpath: Path, exe: Path):
    """The release goes into Play/ (the user's own install, gitignored) before it's published, exactly as the launcher's
    Update would install it: the mod's files are replaced, the player's accounts, settings and state stay."""
    play = ROOT / "Play"
    if not play.is_dir():
        print("No Play folder: the release wasn't installed for a local test.")
        return
    written = kept = 0
    try:
        with zipfile.ZipFile(zpath) as z:
            for entry in z.infolist():
                name = entry.filename.replace("\\", "/")
                if not name.startswith("mod/") or ".." in name or name.endswith("/"):
                    continue
                dest = play / name
                if dest.exists() and name in KEEP_IF_PRESENT:
                    kept += 1
                    continue
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(z.read(entry))
                written += 1
    except PermissionError as e:
        sys.exit(f"Play: {e.filename or 'a file of the mod'} is in use. Close the game and the server started from Play, "
                 f"then run build_release.py --play-only")
    try:
        shutil.copy2(exe, play / exe.name)
        launcher = "and the launcher"
    except PermissionError:
        # the launcher is open (its own .exe can't be replaced while it runs): the mod's files are in all the same
        launcher = f"but not the launcher: {exe.name} is open (close it and run build_release.py --play-only for it)"
    print(f"Play: installed {written} files ({kept} of the player's own kept) {launcher}, for a local test.")


if __name__ == "__main__":
    if "--play-only" in sys.argv:
        # the release already in dist/ into Play, without building it again
        v = (MOD / "VERSION").read_text().strip()
        install_into_play(DIST / f"CSNZ-Offline-Revived-{v}.zip", DIST / "CSNZ-Offline-Revived-Launcher.exe")
    else:
        main()

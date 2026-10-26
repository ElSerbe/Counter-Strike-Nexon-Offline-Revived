"""Changes cstrike-online.exe's requested execution level from RequireAdministrator to asInvoker.

The replacement keeps the exact byte length (padded with spaces inside the XML start tag),
so the PE layout and resource table stay untouched. The original bytes are verified first,
and `revert` restores the file from mod/backup/original/Bin.

Usage:
  python patch_manifest.py <path-to-Bin> apply
  python patch_manifest.py <path-to-Bin> revert
"""
import shutil
import sys
from pathlib import Path

MOD = Path(__file__).resolve().parents[1]
BACKUP = MOD / "backup" / "original" / "Bin" / "cstrike-online.exe"

ORIG = b'level="RequireAdministrator"'
NEW = b'level="asInvoker"' + b" " * (len(ORIG) - len(b'level="asInvoker"'))


def apply(exe: Path) -> None:
    assert len(ORIG) == len(NEW), "replacement must keep the same length"
    data = bytearray(exe.read_bytes())
    count = data.count(ORIG)
    if count == 0 and data.count(NEW) == 1:
        print("[skip] manifest already set to asInvoker")
        return
    if count != 1:
        sys.exit(f"[abort] expected exactly one manifest level string, found {count}")
    i = data.find(ORIG)
    data[i:i + len(ORIG)] = NEW
    exe.write_bytes(bytes(data))
    print(f"[ok] cstrike-online.exe: RequireAdministrator -> asInvoker at offset 0x{i:x}")


def revert(exe: Path) -> None:
    shutil.copy2(BACKUP, exe)
    print("[restored] cstrike-online.exe from backup")


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[2] not in ("apply", "revert"):
        sys.exit(__doc__)
    target = Path(sys.argv[1]) / "cstrike-online.exe"
    apply(target) if sys.argv[2] == "apply" else revert(target)

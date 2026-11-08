"""Runs every export of the in-game web pages' pictures in one Python process (the launcher's Pictures button).

One process means the game's archives are opened once (nartool --serve, shared through export_shop_assets) instead
of once per script and file group: each start of nartool.exe is checked by the antivirus, and thousands of them made
it pop up again and again. Prints "[n/5] <step>" before each step so the launcher can show the progress.
Usage: python -u export_all.py   (CSNZ_GAME = the game's folder)
"""
import importlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

STEPS = [
    ("export_shop_assets", "shop item pictures and 3D models"),
    ("export_bingo_icons", "bingo ticket icon"),
    ("export_bot_catalog", "AI player editor's weapons and characters"),
    ("gen_medals", "medals and their icons"),
    ("gen_missions", "missions and their pictures"),
]


def main():
    import export_shop_assets  # the shared nartool
    start = time.time()
    try:
        for n, (name, what) in enumerate(STEPS, 1):
            print(f"[{n}/{len(STEPS)}] {what}...", flush=True)
            t = time.time()
            module = importlib.import_module(name)
            module.main()
            print(f"[{n}/{len(STEPS)}] done in {time.time() - t:.0f} s", flush=True)
    except RuntimeError as e:
        print("error:", e, flush=True)
        return 1
    finally:
        export_shop_assets._close_servers()
    print(f"Everything exported in {time.time() - start:.0f} s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

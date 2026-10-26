"""Exports the Bingo Ticket icon (gfx/vgui/basket/bingoticket.tga) for the web shop's orders: web/shop/icons/2179.png.

Usage: python export_bingo_icons.py
"""
import os
import tempfile

import export_shop_assets as shop

ICONS = {2179: "bingoticket"}


def main():
    with tempfile.TemporaryDirectory() as tmp:
        shop.run_nartool(["gfx/vgui/basket/%s.tga" % res for res in ICONS.values()], tmp)
        for item_id, res in ICONS.items():
            src = os.path.join(tmp, "gfx", "vgui", "basket", res + ".tga")
            if not os.path.exists(src):
                print("missing:", src)
                continue
            with open(src, "rb") as f:
                png = shop.tga_to_png(f.read())
            dst = os.path.join(shop.ICON_DIR, "%d.png" % item_id)
            with open(dst, "wb") as f:
                f.write(png)
            print("wrote", dst)


if __name__ == "__main__":
    main()

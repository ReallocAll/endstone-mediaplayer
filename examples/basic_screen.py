"""Minimal shipped-package example for the MediaPlayer Python binding."""

import sys

from endstone_mediaplayer import mediaplayer


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "live"
    mp = mediaplayer()
    try:
        screen = mp.screen(name)
        info = screen.info()
        print(f"{info.name}: {info.pixel_width}x{info.pixel_height} pixels")
        screen.clear()
    finally:
        mp.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

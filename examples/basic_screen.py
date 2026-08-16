"""Endstone consumer-plugin example for the MediaPlayer Python binding."""

from endstone.plugin import Plugin

from endstone_mediaplayer import AlreadyExistsError, NotFoundError, mediaplayer


def find_or_create_screen(mp, name: str):
    try:
        return mp.screen(name)
    except NotFoundError:
        try:
            return mp.create_screen(name, 1, 1, backend="logical")
        except AlreadyExistsError:
            return mp.screen(name)


class BasicScreenPlugin(Plugin):
    """All provider calls run in Endstone's main-thread callbacks."""

    depend = ["mediaplayer"]

    def on_enable(self) -> None:
        self._mp = mediaplayer()
        self._screen = find_or_create_screen(self._mp, "python-sdk-demo")

        # RGBA8888 uses direct R,G,B,A bytes with the current provider.
        red = bytes([255, 32, 32, 255]) * (8 * 8)
        blue = bytes([32, 32, 255, 255]) * (8 * 8)
        self._screen.update_region(0, 0, 8, 8, red, format="rgba8888")

        # Normal exit commits both regions together; an exception aborts.
        with self._screen.frame() as frame:
            frame.update_region(0, 8, 8, 8, red, format="rgba8888")
            frame.update_region(8, 8, 8, 8, blue, format="rgba8888")

        info = self._screen.info()
        stats = self._screen.stats()
        self.logger.info(
            f"{info.name}: {info.pixel_width}x{info.pixel_height}, "
            f"generation={stats.generation}, resident={stats.resident_tiles}"
        )

    def on_disable(self) -> None:
        # Closing wrappers does not clear pixels, delete the screen, or unload
        # the provider. Reacquire everything after the next enable/reload.
        if getattr(self, "_mp", None) is not None:
            self._mp.close()
            self._screen = None
            self._mp = None

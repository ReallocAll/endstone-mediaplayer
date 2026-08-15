"""Python bindings for an already-loaded Endstone MediaPlayer provider."""

from ._binding import (
    AlreadyExistsError,
    BusyError,
    Capabilities,
    CapacityError,
    Frame,
    InvalidArgumentError,
    InvalidHandleError,
    MediaPlayer,
    MediaPlayerError,
    MediaPlayerIOError,
    NotFoundError,
    ProviderReloadedError,
    ProviderUnavailableError,
    Screen,
    ScreenInfo,
    Stats,
    acquire,
)


def mediaplayer() -> MediaPlayer:
    """Acquire the active provider; all calls must run on the main thread."""
    return acquire()


__all__ = [
    "AlreadyExistsError",
    "BusyError",
    "Capabilities",
    "CapacityError",
    "Frame",
    "InvalidArgumentError",
    "InvalidHandleError",
    "MediaPlayer",
    "MediaPlayerError",
    "MediaPlayerIOError",
    "NotFoundError",
    "ProviderReloadedError",
    "ProviderUnavailableError",
    "Screen",
    "ScreenInfo",
    "Stats",
    "mediaplayer",
]

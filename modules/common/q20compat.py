# Mercury / Badgeware Q20 compatibility layer.
#
# Firmware v0.7.3.
#
# q20system remains the only physical reader. This module patches the public
# bbq20kbd.BBQ20Keyboard class after q20system has already created the real
# hardware instance, so existing apps keep their old source code while reading
# from q20system's shared event/motion queue instead of racing the hardware FIFO.

import builtins

import q20system


_installed = False
_original_launch = None
_driver_module = None
_physical_keyboard = None
_proxy_class = None


def _physical():
    global _physical_keyboard

    if _physical_keyboard is None:
        _physical_keyboard = q20system.keyboard()
    return _physical_keyboard


def _blank_display_for_sleep():
    # powman.sleep() never returns into the current app; wake is a fresh boot.
    # Blank the framebuffer and kill the backlight before q20system powers off,
    # otherwise the ST7789 remains visibly frozen on its last frame.
    display = getattr(builtins, "display", None)
    screen = getattr(builtins, "screen", None)
    colour = getattr(builtins, "color", None)

    if screen is not None and colour is not None:
        try:
            old_pen = getattr(screen, "pen", None)
            screen.pen = colour.black
            screen.clear()
            if old_pen is not None:
                screen.pen = old_pen
        except Exception:  # noqa: BLE001 - shutdown must remain best-effort
            pass

    if display is not None:
        try:
            display.update()
        except Exception:  # noqa: BLE001 - shutdown must remain best-effort
            pass
        try:
            display.backlight(0.0)
        except Exception:  # noqa: BLE001 - shutdown must remain best-effort
            pass


def _sleep():
    _blank_display_for_sleep()

    # Use the same ordinary button-wake path as Badgeware, never shipping mode.
    # The original q20system function is intentionally bypassed here because it
    # did not blank the LCD first.
    import powman
    powman.sleep()


class _QueuedBBQ20Keyboard:
    """Compatibility facade for legacy Badgeware apps.

    Existing source can continue to do:

        keyboard = BBQ20Keyboard(i2c)
        events = keyboard.update()
        dx = keyboard.dx
        dy = keyboard.dy

    The i2c argument is accepted but q20system owns the real device.
    """

    def __init__(self, i2c=None, *args, **kwargs):
        del i2c, args, kwargs

        self.dx = 0
        self.dy = 0
        self.sx = 0
        self.sy = 0
        self._backlight = None

        keyboard = _physical()
        if keyboard is None:
            raise RuntimeError(q20system.last_error() or "BBQ20KBD not found")

        # Activate q20system's existing application queue. This is the same
        # path Mercury Remote already uses successfully.
        q20system.client_start(
            backlight=None,
            report_mods=False,
            flush=False,
        )

    def update(self, max_events=16):
        incoming = q20system.events(max_events, poll_first=True)
        dx, dy, sx, sy = q20system.consume_motion()

        self.dx = dx
        self.dy = dy
        self.sx = sx
        self.sy = sy

        return incoming

    def flush(self):
        q20system.clear()

        keyboard = _physical()
        if keyboard is not None:
            try:
                keyboard.flush()
            except Exception:  # noqa: BLE001 - legacy driver compatibility
                pass

    @property
    def backlight(self):
        keyboard = _physical()
        if keyboard is not None:
            try:
                return keyboard.backlight
            except Exception:  # noqa: BLE001 - some builds expose setter only
                pass
        return self._backlight

    @backlight.setter
    def backlight(self, value):
        self._backlight = value

        keyboard = _physical()
        if keyboard is not None:
            try:
                keyboard.backlight = value
            except Exception:  # noqa: BLE001 - legacy driver compatibility
                pass

    def __getattr__(self, name):
        # Forward less-common driver features/configuration reads so old apps
        # keep working even if they use more than update()/motion/backlight.
        keyboard = _physical()
        if keyboard is None:
            raise AttributeError(name)
        return getattr(keyboard, name)


def _patch_driver():
    global _driver_module, _proxy_class

    # Important: create the real driver BEFORE replacing the public class.
    keyboard = _physical()
    if keyboard is None:
        return False

    try:
        module = __import__("bbq20kbd")
    except Exception:  # noqa: BLE001 - Q20 is optional system hardware
        return False

    current = getattr(module, "BBQ20Keyboard", None)
    if current is None:
        return False

    # Preserve the original class for diagnostics/recovery, but never create a
    # second physical reader from it once q20system owns the device.
    if getattr(module, "_Q20SYSTEM_ORIGINAL_CLASS", None) is None:
        try:
            module._Q20SYSTEM_ORIGINAL_CLASS = current  # noqa: SLF001
        except Exception:  # noqa: BLE001 - module may reject custom attrs
            pass

    module.BBQ20Keyboard = _QueuedBBQ20Keyboard
    _driver_module = module
    _proxy_class = _QueuedBBQ20Keyboard
    return True


def _wrap_launch():
    global _original_launch

    if _original_launch is not None:
        return

    launch = getattr(builtins, "launch", None)
    if not callable(launch):
        return

    _original_launch = launch

    def compat_launch(path):
        # Do not carry stale app events between apps.
        q20system.client_stop()
        try:
            return _original_launch(path)
        finally:
            q20system.client_stop()

    builtins.launch = compat_launch


def install():
    global _installed

    if _installed:
        return True

    # q20system itself was installed first from modules/common/main.py.
    # Its existing physical keyboard object and global system chords stay
    # unchanged; only the legacy public driver path is redirected.
    if not _patch_driver():
        return False

    # Replace only the sleep action. _dispatch_event resolves request_sleep
    # from q20system's module globals at runtime, so BB+Hangup now blanks first.
    q20system.request_sleep = _sleep

    _wrap_launch()

    _installed = True
    return True

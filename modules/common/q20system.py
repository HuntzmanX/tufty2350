# Badgeware global BBQ20/Q20 service.
#
# Owns the Q20 key FIFO so system-wide BB chords can coexist with apps that
# need the full keyboard stream. Apps opt in with client_start() and consume
# queued events/motion with events() / consume_motion().

import sys
import time
import builtins
import machine
import powman

Q20_ADDRESS = 0x1F
RAW_BB = 0x12
RAW_BACK = 0x11
RAW_HANGUP = 0x07

REG_CFG = 0x02
REG_CFG_WRITE = REG_CFG | 0x80
CFG_REPORT_MODS = 1 << 6

_RETRY_MS = 5000
_QUEUE_LIMIT = 64

_keyboard = None
_i2c = None
_last_probe_ms = -_RETRY_MS
_last_error = None
_client_active = False
_event_queue = []
_motion_x = 0
_motion_y = 0
_motion_sx = 0
_motion_sy = 0

_bb_held = False
_bb_passthrough = False
_bb_consumed = False
_pending_bb = None

_installed = False
_original_badge_poll = None
_original_launch = None
_active_path = None


def _ticks_ms():
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.time() * 1000)


def _ticks_diff(a, b):
    try:
        return time.ticks_diff(a, b)
    except AttributeError:
        return a - b


def _candidate_values(event):
    return (
        getattr(event, "keycode", None),
        getattr(event, "raw_char", None),
        getattr(event, "char", None),
        getattr(event, "key", None),
    )


def _raw_code(value):
    if isinstance(value, int):
        return value & 0xFF
    if isinstance(value, str) and len(value) == 1:
        try:
            return ord(value) & 0xFF
        except Exception:  # noqa: BLE001 - driver values are intentionally loose
            return None
    return None


def _has_raw(event, code):
    for value in _candidate_values(event):
        if _raw_code(value) == code:
            return True
    return False


def _is_bb(event):
    return _has_raw(event, RAW_BB)


def _is_back(event):
    return _has_raw(event, RAW_BACK)


def _is_hangup(event):
    return _has_raw(event, RAW_HANGUP)


def _pressed(event):
    return bool(getattr(event, "pressed", False))


def _released(event):
    return bool(getattr(event, "released", False))


def _queue(event):
    if not _client_active:
        return
    if len(_event_queue) >= _QUEUE_LIMIT:
        del _event_queue[0]
    _event_queue.append(event)


def _reset_bb_state():
    global _bb_held, _bb_passthrough, _bb_consumed, _pending_bb
    _bb_held = False
    _bb_passthrough = False
    _bb_consumed = False
    _pending_bb = None


def _request_home():
    # Mirror Badgeware launch()'s do_exit() before resetting to the launcher.
    # _active_path is maintained by the launch wrapper installed at boot.
    try:
        path = _active_path
        if path and path in sys.modules:
            app = sys.modules[path]
            on_exit = getattr(app, "on_exit", None)
            if callable(on_exit):
                on_exit()
    finally:
        reset = getattr(builtins, "reset", None)
        if callable(reset):
            reset()
        else:
            machine.reset()


def request_home():
    _request_home()


def request_sleep():
    # Badgeware's ordinary button-wake sleep, not shipping mode.
    powman.sleep()


def _dispatch_event(event):
    global _bb_held, _bb_passthrough, _bb_consumed, _pending_bb

    pressed = _pressed(event)
    released = _released(event)

    if _is_bb(event):
        if pressed:
            _bb_held = True
            _bb_passthrough = False
            _bb_consumed = False
            _pending_bb = event
            return

        if released:
            if _bb_held and not _bb_consumed:
                if not _bb_passthrough and _pending_bb is not None:
                    _queue(_pending_bb)
                _queue(event)
            _reset_bb_state()
            return

        return

    if _bb_held:
        # These two chords belong to Badgeware itself and never reach an app.
        if pressed and _is_back(event):
            _bb_consumed = True
            _pending_bb = None
            _request_home()
            return

        if pressed and _is_hangup(event):
            _bb_consumed = True
            _pending_bb = None
            request_sleep()
            return

        if _bb_consumed:
            return

        # The first non-system key proves this was a BB modifier chord. Release
        # the buffered BB-down event to the active client before the chord key.
        if not _bb_passthrough:
            _bb_passthrough = True
            if _pending_bb is not None:
                _queue(_pending_bb)
                _pending_bb = None

        _queue(event)
        return

    _queue(event)


def _ensure_system_path():
    if "/system" not in sys.path:
        sys.path.insert(0, "/system")


def _ensure_keyboard(force=False):
    global _keyboard, _i2c, _last_probe_ms, _last_error

    if _keyboard is not None:
        return _keyboard

    now = _ticks_ms()
    if not force and _ticks_diff(now, _last_probe_ms) < _RETRY_MS:
        return None
    _last_probe_ms = now

    try:
        _ensure_system_path()
        from bbq20kbd import BBQ20Keyboard
    except Exception as exc:  # noqa: BLE001 - optional external driver
        _last_error = "driver: " + str(exc)
        return None

    candidates = (
        (0, 4, 5),
        (1, 6, 7),
        (0, 20, 21),
        (1, 26, 27),
    )

    for bus, sda_pin, scl_pin in candidates:
        try:
            i2c = machine.I2C(
                bus,
                sda=machine.Pin(sda_pin),
                scl=machine.Pin(scl_pin),
                freq=100000,
            )
            if Q20_ADDRESS not in i2c.scan():
                continue

            keyboard = BBQ20Keyboard(i2c)
            _keyboard = keyboard
            _i2c = i2c
            _last_error = None
            return keyboard
        except Exception as exc:  # noqa: BLE001 - probe each candidate bus safely
            _last_error = str(exc)

    return None


def enable_modifier_reporting():
    global _last_error
    keyboard = _ensure_keyboard(force=True)
    if keyboard is None or _i2c is None:
        return False
    try:
        _i2c.writeto(Q20_ADDRESS, bytes((REG_CFG,)))
        current = _i2c.readfrom(Q20_ADDRESS, 1)[0]
        wanted = current | CFG_REPORT_MODS
        if wanted != current:
            _i2c.writeto(Q20_ADDRESS, bytes((REG_CFG_WRITE, wanted)))
        return True
    except Exception as exc:  # noqa: BLE001 - I2C hardware can fail dynamically
        _last_error = str(exc)
        return False


def install():
    """Install the global Q20 poller and launch-path tracker once."""
    global _installed, _original_badge_poll, _original_launch, _active_path

    if _installed:
        return True

    badge = getattr(builtins, "badge", None)
    launch = getattr(builtins, "launch", None)

    if badge is None or not callable(getattr(badge, "poll", None)):
        return False
    if not callable(launch):
        return False

    _original_badge_poll = badge.poll
    _original_launch = launch

    def system_poll():
        _original_badge_poll()
        poll()

    def tracked_launch(path):
        global _active_path
        previous = _active_path
        _active_path = path
        try:
            return _original_launch(path)
        finally:
            _active_path = previous

    badge.poll = system_poll
    builtins.launch = tracked_launch
    _installed = True
    return True


def client_start(backlight=None, report_mods=True, flush=True):
    global _client_active, _motion_x, _motion_y, _motion_sx, _motion_sy

    keyboard = _ensure_keyboard(force=True)
    if keyboard is None:
        return None

    _client_active = True
    _event_queue[:] = []
    _motion_x = 0
    _motion_y = 0
    _motion_sx = 0
    _motion_sy = 0
    _reset_bb_state()

    if flush:
        try:
            keyboard.flush()
        except Exception:  # noqa: BLE001 - legacy driver compatibility
            pass

    if report_mods:
        enable_modifier_reporting()
        if flush:
            try:
                keyboard.flush()
            except Exception:  # noqa: BLE001 - legacy driver compatibility
                pass

    if backlight is not None:
        try:
            keyboard.backlight = backlight
        except Exception:  # noqa: BLE001 - some driver builds vary here
            pass

    return keyboard


def client_stop():
    global _client_active, _motion_x, _motion_y, _motion_sx, _motion_sy
    _client_active = False
    _event_queue[:] = []
    _motion_x = 0
    _motion_y = 0
    _motion_sx = 0
    _motion_sy = 0
    _reset_bb_state()


def client_active():
    return _client_active


def keyboard():
    return _ensure_keyboard(force=True)


def last_error():
    return _last_error


def poll(max_events=16):
    global _motion_x, _motion_y, _motion_sx, _motion_sy, _last_error

    keyboard = _ensure_keyboard()
    if keyboard is None:
        return False

    try:
        incoming = keyboard.update(int(max_events))
    except Exception as exc:  # noqa: BLE001 - hardware poll must not crash Badgeware
        _last_error = str(exc)
        return False

    # The driver clears its movement registers on update(), so accumulate them
    # here. A client can then consume motion even if badge.poll() ran between
    # two app frames.
    if _client_active:
        try:
            _motion_x += int(getattr(keyboard, "dx", 0))
            _motion_y += int(getattr(keyboard, "dy", 0))
            _motion_sx += int(getattr(keyboard, "sx", 0))
            _motion_sy += int(getattr(keyboard, "sy", 0))
        except Exception:  # noqa: BLE001 - driver motion attrs are optional
            pass

    if incoming:
        for event in incoming:
            _dispatch_event(event)

    return True


def events(max_events=16, poll_first=True):
    if poll_first:
        poll(max_events)

    count = min(max(0, int(max_events)), len(_event_queue))
    if count <= 0:
        return []

    result = _event_queue[:count]
    del _event_queue[:count]
    return result


def consume_motion():
    global _motion_x, _motion_y, _motion_sx, _motion_sy
    values = (_motion_x, _motion_y, _motion_sx, _motion_sy)
    _motion_x = 0
    _motion_y = 0
    _motion_sx = 0
    _motion_sy = 0
    return values


def clear():
    global _motion_x, _motion_y, _motion_sx, _motion_sy
    _event_queue[:] = []
    _motion_x = 0
    _motion_y = 0
    _motion_sx = 0
    _motion_sy = 0
    _reset_bb_state()

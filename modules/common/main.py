import os
import powman

if powman.get_wake_reason() == powman.WAKE_DOUBLETAP:
    import _msc  # noqa: F401


try:
    os.listdir("/system")
except OSError:
    fatal_error("System Error!", "Unable to mount filesystem. This may be a temporary error, try resetting your board!")


# Mercury / BBQ20 system layer. This patches Badgeware's shared poll() and
# launch() hooks without replacing the user's /system filesystem.
try:
    import q20system
    q20system.install()
except Exception as exc:  # noqa: BLE001 - Q20 is optional system hardware
    print("Q20 system service unavailable:", exc)


# Firmware v0.7.3 compatibility layer:
# - routes legacy BBQ20Keyboard users through q20system's queue
# - blanks the ST7789/backlight before BB+Hangup sleep
try:
    import q20compat
    q20compat.install()
except Exception as exc:  # noqa: BLE001 - never block Badgeware boot
    print("Q20 compatibility service unavailable:", exc)


try:
    with open("hardware_test.txt", "r"):
        import hardware_test   # noqa F401
except OSError:
    pass


try:
    __import__("/system/main")
except ImportError:
    fatal_error("System Error!", "Could not find main.py. Please double-tap RESET to switch into USB Mass Storage mode and replace it!")

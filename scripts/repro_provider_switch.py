"""Repro: drive the workx TUI to test provider switching via console input injection.

Usage: python scripts/repro_provider_switch.py
Sends: /provider Enter, then ArrowDown Enter (switch to 2nd provider), then checks log.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

# --- console input structs (Windows) ---
KEY_EVENT = 0x0001
VK_RETURN = 0x0D
VK_DOWN = 0x28
VK_UP = 0x26
VK_ESCAPE = 0x1B


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("bKeyDown", wt.BOOL),
        ("wRepeatCount", wt.WORD),
        ("wVirtualKeyCode", wt.WORD),
        ("wVirtualScanCode", wt.WORD),
        ("uChar", ctypes.c_wchar),
        ("dwControlKeyState", wt.DWORD),
    ]


class INPUT_RECORD(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("KeyEvent", KEY_EVENT_RECORD)]
    _anonymous_ = ("u",)
    _fields_ = [("EventType", wt.WORD), ("u", _U)]


def inject_key(pid, vk, char=None, down=True):
    """Write a key event to the console input buffer of the given PID."""
    k = INPUT_RECORD()
    k.EventType = KEY_EVENT
    k.KeyEvent.bKeyDown = down
    k.KeyEvent.wRepeatCount = 1
    k.KeyEvent.wVirtualKeyCode = vk
    if char is not None:
        k.KeyEvent.uChar = char
    n = wt.DWORD(0)
    if not ctypes.windll.kernel32.WriteConsoleInputW(
            ctypes.c_void_p(stdin_handle), ctypes.byref(k), 1, ctypes.byref(n)):
        raise OSError(ctypes.get_last_error())
    time.sleep(0.03)


def tap(pid, vk, char=None):
    """Send keydown + keyup (complete key pair) for a key."""
    inject_key(pid, vk, char, down=True)
    inject_key(pid, vk, char, down=False)


def inject_text(pid, text):
    for ch in text:
        tap(pid, 0, ch)


def find_process():
    """Try to attach to the console of the workx process."""
    # Use a temporary env var to pass PID; simpler: find by name
    return None


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else None
    if pid is None:
        print("usage: repro_provider_switch.py <pid>")
        return
    global stdin_handle
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.FreeConsole()  # detach from our own console before attaching to target
    if not kernel32.AttachConsole(pid):
        print("AttachConsole failed:", ctypes.get_last_error())
        return
    # Reliable way to get the attached console's input handle
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    stdin_handle = kernel32.CreateFileW(
        "CONIN$", GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
    if stdin_handle == -1:
        print("CreateFile CONIN$ failed:", ctypes.get_last_error())
        return

    # Type /provider
    inject_text(pid, "/provider")
    tap(pid, VK_RETURN)
    time.sleep(1.5)
    # Move down one (from current provider) and press Enter to activate
    tap(pid, VK_DOWN)
    time.sleep(0.3)
    tap(pid, VK_RETURN)
    time.sleep(2.0)
    ctypes.windll.kernel32.FreeConsole()
    print("done")


if __name__ == "__main__":
    main()

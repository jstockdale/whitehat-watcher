"""Host-side OS input injection (pynput).

This is the *fallback* input path: when the Watcher cannot drive the host
over BLE HID (not paired, out of range), the host agent can inject input
itself. ``click_at`` is also how Phase 0 automation flows execute a
"click the button" decision on the host side.

Cross-platform: pynput works on macOS and Linux. Notes:
  - macOS requires the terminal / Python to be granted Accessibility
    permission (System Settings -> Privacy & Security -> Accessibility),
    otherwise injection silently does nothing.
  - On Linux this needs a running X session (X11). Wayland support via
    pynput is limited; prefer X11 or the BLE HID path on Wayland.

pynput is imported lazily so that importing this module never crashes on a
headless machine; failures raise RuntimeError with an actionable message.
"""

from __future__ import annotations


def _controllers():
    """Return (mouse, keyboard, MouseButton, Key), or raise RuntimeError."""
    try:
        from pynput.mouse import Controller as MouseController, Button as MouseButton
        from pynput.keyboard import Controller as KeyboardController, Key
    except Exception as exc:
        raise RuntimeError(
            "pynput is required for host-side input injection "
            "(pip install -r requirements.txt). "
            "On headless Linux, pynput also needs a running X display."
        ) from exc
    return MouseController(), KeyboardController(), MouseButton, Key


def _screen_size() -> tuple:
    """Primary screen size in pixels via tkinter (stdlib, cross-platform)."""
    try:
        import tkinter as tk

        root = tk.Tk()
        root.withdraw()
        size = (root.winfo_screenwidth(), root.winfo_screenheight())
        root.destroy()
        return size
    except Exception as exc:
        raise RuntimeError(
            "Could not determine screen size: tkinter needs a display. "
            "Run on the desktop session, not over headless SSH."
        ) from exc


def _key_for(code: str, Key):
    """Map a protocol key code to a pynput key. Raises ValueError if unknown."""
    from protocol import KEY_CODES

    if code not in KEY_CODES:
        raise ValueError(f"Unknown key code {code!r}")
    if len(code) == 1:  # letters a-z, digits 0-9
        return code
    named = {
        "enter": Key.enter,
        "tab": Key.tab,
        "esc": Key.esc,
        "space": Key.space,
        "backspace": Key.backspace,
        "delete": Key.delete,
        "home": Key.home,
        "end": Key.end,
        "pgup": Key.page_up,
        "pgdn": Key.page_down,
        "up": Key.up,
        "down": Key.down,
        "left": Key.left,
        "right": Key.right,
    }
    if code in named:
        return named[code]
    if code.startswith("f") and code[1:].isdigit() and 1 <= int(code[1:]) <= 12:
        return getattr(Key, code)  # Key.f1 .. Key.f12
    raise ValueError(f"Key code {code!r} has no pynput mapping")


_MODIFIERS = {"ctrl": "ctrl", "shift": "shift", "alt": "alt", "gui": "cmd"}


def press_key(code: str, mod: str | None = None) -> None:
    """Press a key with optional '+'-joined modifiers (same vocabulary as the protocol)."""
    from protocol import KEY_MODIFIERS

    mouse, keyboard, _Button, Key = _controllers()
    mods = []
    if mod:
        for name in mod.split("+"):
            if name not in KEY_MODIFIERS:
                raise ValueError(f"Unknown modifier {name!r}")
            mods.append(getattr(Key, _MODIFIERS[name]))
    key = _key_for(code, Key)
    for m in mods:
        keyboard.press(m)
    keyboard.press(key)
    keyboard.release(key)
    for m in reversed(mods):
        keyboard.release(m)


def type_text(text: str) -> None:
    """Type text as if on the host keyboard."""
    _mouse, keyboard, _Button, _Key = _controllers()
    keyboard.type(str(text))


def move(dx: int, dy: int) -> None:
    """Move the host pointer by a relative (dx, dy) pixel offset."""
    mouse, _keyboard, _Button, _Key = _controllers()
    mouse.move(int(dx), int(dy))


def click(button: str = "left") -> None:
    """Click a mouse button at the current pointer position."""
    from protocol import MOUSE_BUTTONS

    if button not in MOUSE_BUTTONS:
        raise ValueError(f"Unknown mouse button {button!r}")
    mouse, _keyboard, MouseButton, _Key = _controllers()
    mouse.click(getattr(MouseButton, button))


def click_at(x_norm: float, y_norm: float) -> None:
    """Absolute click at normalized screen coordinates (0..1, 0..1).

    This is the Phase 0 primitive automation flows use to execute a
    "click the button" decision: vision (or the operator) produces
    normalized coordinates, and this performs the click on the host.
    """
    if not (0.0 <= x_norm <= 1.0 and 0.0 <= y_norm <= 1.0):
        raise ValueError("normalized coordinates must be in [0, 1]")
    width, height = _screen_size()
    mouse, _keyboard, MouseButton, _Key = _controllers()
    mouse.position = (int(x_norm * width), int(y_norm * height))
    mouse.click(MouseButton.left)

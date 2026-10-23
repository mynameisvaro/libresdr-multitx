#!/usr/bin/env python3
"""A gamepad on the PC as the sticks and switches of a handset (for multi_link.py).

Reads the gamepad with pygame (Linux, macOS, Windows; `pip install pygame`). Pads
SDL knows (Xbox, PlayStation, Switch Pro and most clones) get standard names:

  axes     leftx lefty rightx righty lefttrigger righttrigger
  buttons  a b x y back guide start leftstick rightstick leftshoulder
           rightshoulder dpup dpdown dpleft dpright

Other joysticks use raw names: axis0.., button0.., hat0x, hat0y.

A mapping assigns one input to each channel (1..16), Multiprotocol's AETR order:

  1=rightx 2=-righty 3=-lefty 4=leftx       (the default: mode 2)
  5=a:toggle 6=b:toggle 7=x:toggle 8=y:toggle 9=lefttrigger 10=righttrigger

"-" inverts; a button gives -100% / +100% while held, ":toggle" flips on each
press like a latching switch; a trigger goes from -100% (released) to +100%.
Holding "back" sets the bind flag (multi_link.py --bind-button).

  gamepad.py            show the gamepads and their inputs live, to find the names
"""

import os
import sys
import time

DEFAULT_MAP = ("1=rightx 2=-righty 3=-lefty 4=leftx 5=a:toggle 6=b:toggle "
               "7=x:toggle 8=y:toggle 9=lefttrigger 10=righttrigger")
CONTROLLER_AXES = ["leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"]
CONTROLLER_BUTTONS = ["a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
                      "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"]
TRIGGERS = ("lefttrigger", "righttrigger")

CH_MIN, CH_MID, CH_MAX = 204, 1024, 1844        # -100%, 0, +100% (Multiprotocol)


def _pygame():
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    os.environ.setdefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")
    if sys.platform.startswith("linux") and not (os.environ.get("DISPLAY") or
                                                 os.environ.get("WAYLAND_DISPLAY")):
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")   # headless: events still work
    import pygame
    pygame.init()
    pygame.joystick.init()
    return pygame


class Mapping:
    """Turns gamepad inputs into 16 channel values (0..2047). Pure: feed it the
    inputs as a dict {name: value}, axes -1..1 (triggers 0..1), buttons bool, and
    optionally "_pressed": the buttons pressed since the last call (so a toggle
    doesn't miss a tap shorter than the frame interval)."""

    def __init__(self, spec=DEFAULT_MAP, deadband=0.04, expo=0.0, endpoint=100):
        self.deadband, self.expo, self.endpoint = deadband, expo, endpoint
        self.entries = {}
        for item in spec.split():
            ch, src = item.split("=")
            ch = int(ch)
            if not 1 <= ch <= 16:
                raise ValueError(f"channel {ch} out of 1..16")
            invert = src.startswith("-")
            name, _, mode = src.lstrip("-").partition(":")
            if mode not in ("", "toggle"):
                raise ValueError(f"unknown mode '{mode}' in '{item}'")
            self.entries[ch] = (name, invert, mode == "toggle")
        self.toggled = {}
        self.prev = {}

    def inputs(self):
        return sorted({name for name, _, _ in self.entries.values()})

    def _axis(self, v, trigger):
        if trigger:
            v = 2 * v - 1                       # 0..1 -> -1..1
        elif abs(v) < self.deadband:
            v = 0.0
        else:                                   # keep the full range after the deadband
            v = (abs(v) - self.deadband) / (1 - self.deadband) * (1 if v > 0 else -1)
        if self.expo:
            v = (1 - self.expo) * v + self.expo * v ** 3
        return v

    def channels(self, state):
        out = [CH_MID] * 16
        for ch, (name, invert, toggle) in self.entries.items():
            v = state.get(name, 0)
            if isinstance(v, bool):
                if toggle:
                    pressed = name in state["_pressed"] if "_pressed" in state else \
                        v and not self.prev.get(name)
                    if pressed:
                        self.toggled[name] = not self.toggled.get(name, False)
                    v = self.toggled.get(name, False)
                x = 1.0 if v else -1.0
            else:
                x = self._axis(float(v), name in TRIGGERS)
            if invert:
                x = -x
            x = max(-1.0, min(1.0, x)) * self.endpoint / 100
            out[ch - 1] = max(0, min(2047, round(CH_MID + x * (CH_MAX - CH_MID))))
        self.prev = {k: v for k, v in state.items() if isinstance(v, bool)}
        return out


class Gamepad:
    """One gamepad through pygame; state() returns {name: value} for Mapping."""

    def __init__(self, index=0):
        pygame = self.pg = _pygame()
        n = pygame.joystick.get_count()
        if index >= n:
            raise RuntimeError(f"no gamepad {index} ({n} found)")
        self.controller = None
        try:
            from pygame._sdl2 import controller
            controller.init()
            if controller.is_controller(index):
                self.controller = controller.Controller(index)
        except ImportError:
            pass
        self.joy = pygame.joystick.Joystick(index)
        self.joy.init()
        self.name = self.joy.get_name()
        self.standard = self.controller is not None

    def connected(self):
        self.pg.event.pump()
        return self.controller.attached() if self.controller else self.joy.get_init()

    def state(self):
        pg = self.pg
        pressed = set()                 # button presses since the last call (events)
        for ev in pg.event.get((pg.JOYBUTTONDOWN, pg.CONTROLLERBUTTONDOWN)):
            if ev.type == pg.JOYBUTTONDOWN:
                pressed.add(f"button{ev.button}")
            elif ev.button < len(CONTROLLER_BUTTONS):
                pressed.add(CONTROLLER_BUTTONS[ev.button])
        pg.event.clear()                # (the rest isn't needed; keep the queue short)
        s = {"_pressed": pressed}
        if self.controller:
            for i, name in enumerate(CONTROLLER_AXES):
                v = self.controller.get_axis(i) / 32767
                s[name] = max(0.0, v) if name in TRIGGERS else max(-1.0, min(1.0, v))
            for i, name in enumerate(CONTROLLER_BUTTONS):
                s[name] = bool(self.controller.get_button(i))
        j = self.joy
        for i in range(j.get_numaxes()):
            s[f"axis{i}"] = j.get_axis(i)
        for i in range(j.get_numbuttons()):
            s[f"button{i}"] = bool(j.get_button(i))
        for i in range(j.get_numhats()):
            hx, hy = j.get_hat(i)
            s[f"hat{i}x"], s[f"hat{i}y"] = float(hx), float(hy)
        return s


def show():
    pygame = _pygame()
    n = pygame.joystick.get_count()
    print(f"{n} gamepad(s)")
    if not n:
        return 1
    pads = [Gamepad(i) for i in range(n)]
    for i, p in enumerate(pads):
        print(f"  {i}: {p.name} ({'standard names' if p.standard else 'raw names only'})")
    print("move the sticks and press buttons (Ctrl-C to leave)")
    try:
        while True:
            s = pads[0].state()
            s.pop("_pressed")
            active = [f"{k}={v:+.2f}" if not isinstance(v, bool) else k
                      for k, v in s.items() if (v if isinstance(v, bool) else abs(v) > 0.1)]
            sys.stdout.write("\r" + " ".join(active)[:160].ljust(160))
            sys.stdout.flush()
            time.sleep(0.05)
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(show())

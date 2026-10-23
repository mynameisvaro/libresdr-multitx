#!/usr/bin/env python3
"""Unit tests for gamepad.Mapping (no gamepad or board needed): python3 test_gamepad.py"""

import unittest

from gamepad import CH_MAX, CH_MID, CH_MIN, DEFAULT_MAP, Mapping


class MappingTest(unittest.TestCase):
    def test_default_mode2(self):
        m = Mapping(DEFAULT_MAP, deadband=0)
        # sticks: right full right, right up (SDL y is negative up), left down, left centre
        ch = m.channels({"rightx": 1.0, "righty": -1.0, "lefty": 1.0, "leftx": 0.0,
                         "lefttrigger": 0.0, "righttrigger": 1.0})
        self.assertEqual(ch[:4], [CH_MAX, CH_MAX, CH_MIN, CH_MID])
        self.assertEqual(ch[8:10], [CH_MIN, CH_MAX])          # triggers: released, pressed
        self.assertEqual(ch[10:], [CH_MID] * 6)               # unmapped

    def test_deadband_keeps_full_range(self):
        m = Mapping("1=leftx", deadband=0.1)
        self.assertEqual(m.channels({"leftx": 0.05})[0], CH_MID)
        self.assertEqual(m.channels({"leftx": 1.0})[0], CH_MAX)
        self.assertEqual(m.channels({"leftx": -0.55})[0], round(CH_MID - 0.5 * (CH_MAX - CH_MID)))

    def test_toggle_and_momentary(self):
        m = Mapping("5=a:toggle 6=b")
        seq = [False, True, True, False, True, False]
        got = [m.channels({"a": p, "b": p})[4:6] for p in seq]
        self.assertEqual([g[0] for g in got], [CH_MIN, CH_MAX, CH_MAX, CH_MAX, CH_MIN, CH_MIN])
        self.assertEqual([g[1] for g in got], [CH_MIN, CH_MAX, CH_MAX, CH_MIN, CH_MAX, CH_MIN])

    def test_toggle_from_events(self):
        # a tap between two samples: only the press event shows it
        m = Mapping("5=a:toggle")
        self.assertEqual(m.channels({"a": False, "_pressed": {"a"}})[4], CH_MAX)
        self.assertEqual(m.channels({"a": True, "_pressed": set()})[4], CH_MAX)   # still held
        self.assertEqual(m.channels({"a": False, "_pressed": set()})[4], CH_MAX)
        self.assertEqual(m.channels({"a": True, "_pressed": {"a"}})[4], CH_MIN)

    def test_expo_endpoint_invert(self):
        m = Mapping("1=-leftx", deadband=0, expo=1.0, endpoint=50)
        self.assertEqual(m.channels({"leftx": 0.5})[0], round(CH_MID - 0.125 * 0.5 * (CH_MAX - CH_MID)))
        self.assertEqual(m.channels({"leftx": -1.0})[0], round(CH_MID + 0.5 * (CH_MAX - CH_MID)))

    def test_bad_spec(self):
        with self.assertRaises(ValueError):
            Mapping("17=leftx")
        with self.assertRaises(ValueError):
            Mapping("5=a:latch")


if __name__ == "__main__":
    unittest.main()

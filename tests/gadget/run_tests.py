#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
End-to-end driver test against two emulated guns (guncon3_gadget on dummy_hcd).

Run as root from the repo root, after building src/guncon3.ko and
tests/gadget/guncon3_gadget:

    sudo tests/gadget/run_tests.py

It loads dummy_hcd (num=2), raw_gadget and src/guncon3.ko, and unloads the
ones it loaded when done. Needs python-evdev.

udev/70-guncon3.rules is installed for the run as a runtime rule (under
/run, minus the calibration-restore RUN), both to test it and because without
LIBINPUT_IGNORE_DEVICE a desktop compositor keeps the pointer open, so the
driver never sees the last close.
"""
import fcntl
import os
import re
import select
import struct
import subprocess
import sys
import time

import evdev
from evdev import ecodes as e

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GADGET = os.path.join(ROOT, "tests", "gadget", "guncon3_gadget")
MODULE = os.path.join(ROOT, "src", "guncon3.ko")
DRIVER_SYSFS = "/sys/bus/usb/drivers/guncon3"
RULES = os.path.join(ROOT, "udev", "70-guncon3.rules")
RUNTIME_RULES = "/run/udev/rules.d/70-guncon3-test.rules"

# Bits from src/guncon3_proto.h
TRIGGER, A1, A2, A3 = 0x002000, 0x000004, 0x000002, 0x800000
B1, B2, B3, C1, C2 = 0x000400, 0x000200, 0x400000, 0x008000, 0x000008
OFFSCREEN = 0x000800

PAD_CODES = {TRIGGER: e.BTN_SOUTH, A1: e.BTN_TL, A2: e.BTN_TL2, A3: e.BTN_THUMBL,
             B1: e.BTN_TR, B2: e.BTN_TR2, B3: e.BTN_THUMBR, C1: e.BTN_SELECT, C2: e.BTN_START}
POINTER_CODES = {TRIGGER: e.BTN_LEFT, A1: e.BTN_RIGHT, A2: e.BTN_EXTRA,
                 B1: e.BTN_SIDE, B2: e.BTN_MIDDLE}

EVIOCSKEYCODE = 0x40084504  # _IOW('E', 0x04, unsigned int[2])

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def wait_for(pred, timeout=2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return pred()


def sh(*cmd, check_rc=True):
    return subprocess.run(cmd, check=check_rc, capture_output=True, text=True)


class Gun:
    def __init__(self, index):
        self.index = index
        self.proc = subprocess.Popen([GADGET, f"dummy_udc.{index}"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, bufsize=1)
        self.keys = 0
        self.pointer = self.pad = None

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def state(self, x=0, y=0, z=0, buttons=0, ax=128, ay=128, bx=128, by=128):
        self.send(f"state {x} {y} {z} {buttons:x} {ax} {ay} {bx} {by}")

    def drain(self, timeout=0.0):
        """Consume emulator output; returns the lines read."""
        lines = []
        while select.select([self.proc.stdout], [], [], timeout)[0]:
            line = self.proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            lines.append(line)
            if line.startswith("KEY "):
                self.keys = int(line.split()[1])
            timeout = 0.0
        return lines

    def wait_line(self, prefix, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self.drain(0.1):
                if line.startswith(prefix):
                    return line
        return None

    def find_inputs(self):
        tag = f"dummy_hcd.{self.index}-"
        self.pointer = self.pad = None
        for path in evdev.list_devices():
            try:
                dev = evdev.InputDevice(path)
            except OSError:
                continue  # vanished between listing and opening (unplug)
            if tag not in (dev.phys or ""):
                dev.close()
            elif dev.name == "Namco GunCon 3 Pointer":
                self.pointer = dev
            elif dev.name == "Namco GunCon 3 Pad":
                self.pad = dev
            else:
                dev.close()
        return self.pointer is not None and self.pad is not None

    def close_inputs(self):
        for dev in (self.pointer, self.pad):
            if dev:
                dev.close()
        self.pointer = self.pad = None

    def interface(self):
        for name in os.listdir(DRIVER_SYSFS):
            if re.match(r"\d+-[\d.]+:\d+\.\d+$", name):
                devpath = os.path.realpath(os.path.join(DRIVER_SYSFS, name))
                if f"dummy_hcd.{self.index}" in devpath:
                    return name
        return None

    def sysfs(self, attr):
        return os.path.join(DRIVER_SYSFS, self.interface(), attr)

    def stop(self):
        if self.proc.poll() is None:
            self.send("quit")
            try:
                self.proc.wait(2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def abs_value(dev, code):
    return dev.absinfo(code).value


def keys(dev):
    return set(dev.active_keys())


def module_loaded(name):
    return os.path.isdir(f"/sys/module/{name}")


def test_axes_and_buttons(gun):
    print(f"gun {gun.index}: aim mapping")
    for x, want in ((-32768, 0), (0, 32767), (32767, 65535)):
        gun.state(x=x)
        check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) in (want, want + 1)),
              f"raw x={x} -> ABS_X {want} (got {abs_value(gun.pointer, e.ABS_X)})")
    for y, want in ((32767, 0), (-32768, 65535)):
        gun.state(y=y)
        check(wait_for(lambda: abs_value(gun.pointer, e.ABS_Y) == want),
              f"raw y={y} -> ABS_Y {want} (top is +32767)")

    print(f"gun {gun.index}: buttons")
    for bit, code in PAD_CODES.items():
        gun.state(buttons=bit)
        want_ptr = {POINTER_CODES[bit]} if bit in POINTER_CODES else set()
        check(wait_for(lambda: keys(gun.pad) == {code} and keys(gun.pointer) == want_ptr),
              f"bit {bit:06x} -> pad {e.BTN[code] if code in e.BTN else code}, pointer {want_ptr or '-'}"
              f" (pad {keys(gun.pad)}, pointer {keys(gun.pointer)})")
    gun.state()
    check(wait_for(lambda: not keys(gun.pad) and not keys(gun.pointer)), "all released")

    print(f"gun {gun.index}: offscreen")
    gun.state(x=1000, y=1000, buttons=OFFSCREEN | TRIGGER)
    check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == 0 and abs_value(gun.pointer, e.ABS_Y) == 0
                   and e.BTN_LEFT in keys(gun.pointer)),
          "offscreen pins the pointer to 0,0 and the trigger still fires (reload)")

    print(f"gun {gun.index}: sticks, D-pad, distance")
    gun.state(ax=0, ay=255, bx=10, by=200, z=1234)
    check(wait_for(lambda: abs_value(gun.pad, e.ABS_X) == 0 and abs_value(gun.pad, e.ABS_Y) == 255
                   and abs_value(gun.pad, e.ABS_RX) == 10 and abs_value(gun.pad, e.ABS_RY) == 200),
          "A-stick -> ABS_X/Y, B-stick -> ABS_RX/RY")
    check(wait_for(lambda: keys(gun.pad) == {e.BTN_DPAD_LEFT, e.BTN_DPAD_DOWN}),
          f"A-stick left+down -> D-pad left+down (got {keys(gun.pad)})")
    check(abs_value(gun.pad, e.ABS_DISTANCE) == 1234, "z -> ABS_DISTANCE")
    gun.state(ax=128 + 15, ay=128 - 15)
    check(wait_for(lambda: not keys(gun.pad)), "inside the deadzone -> no D-pad")
    # Real gun, held right: Y drifts to ~175. Must not chatter DPAD_DOWN.
    gun.state(ax=255, ay=175)
    check(wait_for(lambda: keys(gun.pad) == {e.BTN_DPAD_RIGHT}),
          f"right with off-axis drift -> D-pad right only (got {keys(gun.pad)})")
    gun.state(ax=128 + 40, ay=128)
    check(wait_for(lambda: abs_value(gun.pad, e.ABS_X) == 168) and keys(gun.pad) == {e.BTN_DPAD_RIGHT},
          "easing back past the press threshold keeps the D-pad held (hysteresis)")
    gun.state(ax=128 + 20, ay=128)
    check(wait_for(lambda: not keys(gun.pad)), "back inside half the deadzone -> released")
    # Real guns send one all-zero report right after the key.
    gun.state(x=500, y=600)
    wait_for(lambda: abs_value(gun.pointer, e.ABS_X) != 32768)
    before = (abs_value(gun.pointer, e.ABS_X), abs_value(gun.pad, e.ABS_X))
    gun.state(ax=0, ay=0, bx=0, by=0)
    time.sleep(0.15)
    check((abs_value(gun.pointer, e.ABS_X), abs_value(gun.pad, e.ABS_X)) == before and not keys(gun.pad),
          "an all-zero report is ignored (no D-pad up-left, aim unchanged)")
    gun.state()


def test_corrupt_reports(gun):
    print(f"gun {gun.index}: corrupt reports")
    gun.state(x=-32768)
    wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == 0)
    gun.drain()
    keys_before = gun.keys
    gun.send("corrupt 3")
    time.sleep(0.2)
    gun.state(x=32767)
    check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == 65535),
          "input keeps flowing after a few bad checksums (upstream stalled here)")
    gun.drain(0.2)
    check(gun.keys == keys_before, "a few bad reports do not trigger a key re-send")

    gun.send("corrupt 20")
    gun.wait_line(f"KEY {keys_before + 1}", 3)
    check(gun.keys > keys_before, f"a run of bad reports re-sends the key ({keys_before} -> {gun.keys})")
    gun.state(x=0)
    check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) in (32767, 32768)),
          "input recovers after the re-send")


def test_calibration(gun):
    print(f"gun {gun.index}: sysfs calibration")
    cal = gun.sysfs("calibration")
    with open(cal) as f:
        check(f.read().split() == ["-32768", "32767", "32767", "-32768", "0", "0"], "default calibration")
    with open(cal, "w") as f:
        f.write("-1000 1000 1000 -1000")
    with open(cal) as f:
        check(f.read().split() == ["-1000", "1000", "1000", "-1000", "0", "0"],
              "4 values: centre defaults to the midpoint")
    for x, want in ((-1000, 0), (0, 32768), (1000, 65535), (-5000, 0), (5000, 65535)):
        gun.state(x=x)
        check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == want),
              f"calibrated x={x} -> {want} (clamped outside the window)")
    # A real gun's calibration (2026-11-07): centre ~6% off the corners' midpoint.
    with open(cal, "w") as f:
        f.write("-16190 16008 520 -12752 -1910 -6410")
    for x, y, want_x, want_y in ((-16190, 520, 0, 0), (-1910, -6410, 32768, 32768),
                                 (16008, -12752, 65535, 65535), (-9050, -2945, 16384, 16384),
                                 (7049, -9581, 49151, 49151)):
        gun.state(x=x, y=y)
        check(wait_for(lambda: (abs_value(gun.pointer, e.ABS_X), abs_value(gun.pointer, e.ABS_Y))
                       == (want_x, want_y)),
              f"centre-aware: raw {x},{y} -> {want_x},{want_y} "
              f"(got {abs_value(gun.pointer, e.ABS_X)},{abs_value(gun.pointer, e.ABS_Y)})")
    gun.state(x=123, y=-456, z=7, buttons=TRIGGER)
    time.sleep(0.1)
    with open(gun.sysfs("raw")) as f:
        raw = f.read().split()
    check(raw[:3] == ["123", "-456", "7"] and int(raw[3], 16) == TRIGGER, f"raw shows uncalibrated input ({raw})")
    for bad in ("1 1 2 3", "garbage", "0 1 2", "-1000 1000 1000 -1000 0",
                "-1000 1000 1000 -1000 1500 0", "-1000 1000 1000 -1000 0 1000"):
        try:
            with open(cal, "w") as f:
                f.write(bad)
            check(False, f"rejects {bad!r}")
        except OSError:
            check(True, f"rejects {bad!r}")
    with open(cal, "w") as f:
        f.write("reset")
    with open(cal) as f:
        check(f.read().split() == ["-32768", "32767", "32767", "-32768", "0", "0"], "reset restores defaults")
    gun.state()


def test_remap(gun):
    print(f"gun {gun.index}: keymap remap (what udev hwdb does)")
    fcntl.ioctl(gun.pad.fd, EVIOCSKEYCODE, struct.pack("II", 0, e.BTN_EAST))
    gun.state(buttons=TRIGGER)
    check(wait_for(lambda: keys(gun.pad) == {e.BTN_EAST}), f"trigger remapped to BTN_EAST (got {keys(gun.pad)})")
    fcntl.ioctl(gun.pad.fd, EVIOCSKEYCODE, struct.pack("II", 0, e.BTN_SOUTH))
    gun.state()
    wait_for(lambda: not keys(gun.pad))


def test_independent(guns):
    print("two guns are independent")
    a, b = guns
    a.state(x=-32768)
    b.state(x=32767)
    check(wait_for(lambda: abs_value(a.pointer, e.ABS_X) == 0 and abs_value(b.pointer, e.ABS_X) == 65535),
          "each pointer follows its own gun")
    with open(a.sysfs("calibration"), "w") as f:
        f.write("-1000 1000 1000 -1000")
    with open(b.sysfs("calibration")) as f:
        check(f.read().split()[0] == "-32768", "calibrating gun 0 leaves gun 1 alone")
    with open(a.sysfs("calibration"), "w") as f:
        f.write("reset")


def test_reopen(gun):
    print(f"gun {gun.index}: close and reopen")
    gun.drain()
    before = gun.keys
    gun.close_inputs()
    time.sleep(0.2)
    check(gun.find_inputs(), "devices still there")
    wait_for(lambda: (gun.drain(0.05), gun.keys > before)[1], 3)
    check(gun.keys > before, "reopening sends the key again")
    gun.state(x=32767)
    check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == 65535), "input flows after reopen")


def test_rebind(gun):
    print(f"gun {gun.index}: unbind/rebind while open (RPCS3 passthrough path)")
    intf = gun.interface()
    with open(os.path.join(DRIVER_SYSFS, "unbind"), "w") as f:
        f.write(intf)
    check(wait_for(lambda: gun.interface() is None), "unbound")
    gun.close_inputs()
    check(not gun.find_inputs(), "input devices removed")
    with open(os.path.join(DRIVER_SYSFS, "bind"), "w") as f:
        f.write(intf)
    check(wait_for(gun.find_inputs), "rebound, input devices back")
    gun.state(x=-32768)
    check(wait_for(lambda: abs_value(gun.pointer, e.ABS_X) == 0), "input flows after rebind")


def test_unplug(gun):
    print(f"gun {gun.index}: unplug while open")
    gun.proc.kill()
    gun.proc.wait()
    gun.close_inputs()
    check(wait_for(lambda: not gun.find_inputs(), 5), "input devices removed after unplug")


def main():
    if os.geteuid() != 0:
        sys.exit("run as root")
    for path in (GADGET, MODULE):
        if not os.path.exists(path):
            sys.exit(f"missing {path}: build it first")
    if module_loaded("guncon3"):
        sys.exit("guncon3 is already loaded; rmmod it first so the test uses src/guncon3.ko")

    with open(RULES) as f:
        rules = "".join(l for l in f if "RUN+=" not in l)
    os.makedirs(os.path.dirname(RUNTIME_RULES), exist_ok=True)
    with open(RUNTIME_RULES, "w") as f:
        f.write(rules)
    sh("udevadm", "control", "--reload")

    loaded = []
    if not module_loaded("dummy_hcd"):
        sh("modprobe", "dummy_hcd", "num=2")
        loaded.append("dummy_hcd")
    if not module_loaded("raw_gadget"):
        sh("modprobe", "raw_gadget")
        loaded.append("raw_gadget")
    sh("insmod", MODULE)
    loaded.append("guncon3")
    dmesg_mark = sh("dmesg").stdout.count("\n")

    guns = []
    try:
        guns = [Gun(0), Gun(1)]
        for gun in guns:
            check(gun.wait_line("READY") is not None, f"gun {gun.index} enumerated")
            check(wait_for(gun.find_inputs, 5), f"gun {gun.index}: pointer and pad registered")
            check(wait_for(lambda: (gun.drain(0.05), gun.keys > 0)[1], 5),
                  f"gun {gun.index}: key received on open")

        caps = guns[0].pointer.capabilities()
        check(e.ABS_X in [c for c, _ in caps.get(e.EV_ABS, [])], "pointer has ABS_X")
        udev = sh("udevadm", "info", guns[0].pointer.path).stdout
        check("ID_INPUT_MOUSE=1" in udev, "udev classifies the pointer as a mouse")
        check("LIBINPUT_IGNORE_DEVICE=1" in udev, "rule hides the pointer from libinput")
        check("uaccess" in udev, "rule gives the logged-in user the pointer (uaccess)")
        udev = sh("udevadm", "info", guns[0].pad.path).stdout
        check("ID_INPUT_JOYSTICK=1" in udev, "udev classifies the pad as a joystick")

        for gun in guns:
            test_axes_and_buttons(gun)
        test_corrupt_reports(guns[0])
        test_calibration(guns[0])
        test_remap(guns[0])
        test_independent(guns)
        test_reopen(guns[1])
        test_rebind(guns[0])
        test_unplug(guns[1])
    finally:
        for gun in guns:
            gun.close_inputs()
            gun.stop()
        time.sleep(0.5)
        new = "\n".join(sh("dmesg").stdout.splitlines()[dmesg_mark:])
        bad = [l for l in new.splitlines() if re.search(r"BUG|WARNING|Call Trace|Oops|general protection", l)]
        check(not bad, "no kernel warnings in dmesg" + ("".join("\n      " + l for l in bad[:10])))
        for mod in reversed(loaded):
            sh("rmmod", mod, check_rc=False)
        os.unlink(RUNTIME_RULES)
        sh("udevadm", "control", "--reload")

    print(f"\n{len(failures)} failure(s)" if failures else "\nall driver tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

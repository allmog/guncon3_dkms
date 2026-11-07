# GunCon 3 USB Lightgun Driver

A DKMS kernel driver for the Namco GunCon 3 (`0b9a:0800`), aimed at RetroArch and
RPCS3 on Arch-based systems (developed on CachyOS, gaming mode under gamescope).
Supports two guns at once.

Fork of [pcnimdock/guncon3_dkms](https://github.com/pcnimdock/guncon3_dkms), which in
turn builds on beardypig's GunCon 2 driver and GunCon 3 protocol work.

> **Status:** works on real hardware (two guns, CachyOS) and is tested end to end
> against an emulated gun (see [Testing](#testing)). Details and what is still open:
> [Hardware status](#hardware-status).

## What it provides

Each gun shows up as two input devices:

| Device | What it is | Used by |
|---|---|---|
| `Namco GunCon 3 Pointer` | Absolute mouse: aim on `ABS_X`/`ABS_Y` (0..65535, calibrated), Trigger = left button, A1 = right, B2 = middle, B1 = side, A2 = extra | RetroArch's udev lightgun driver |
| `Namco GunCon 3 Pad` | Gamepad: all 9 buttons, A-stick on `ABS_X/Y`, B-stick on `ABS_RX/RY`, distance on `ABS_DISTANCE`, A-stick also as D-pad | Menus, button binds, anything that wants a pad |

Pad buttons follow RPCS3's default GunCon 3 layout: Trigger = South (Cross),
A1/A2/A3 = L1/L2/L3, B1/B2/B3 = R1/R2/R3, C1 = Select, C2 = Start.

- **Offscreen:** while the gun cannot see the sensor bar, the pointer reports 0,0
  (the edge), which emulators treat as offscreen. Shooting offscreen reloads.
- **Calibration** is per gun, in the driver, through sysfs, so it works under gamescope,
  Wayland and X11 alike, and is restored by udev for each USB port.
- **Remapping:** button codes can be changed with a udev hwdb file
  ([example](udev/70-guncon3.hwdb.example)).
- **Recovery:** corrupt reports are dropped without stalling input, and a run of them
  re-sends the key to the gun.
- **RPCS3 passthrough:** RPCS3 can hand the real gun to the emulated PS3 (libusb detaches
  this driver while it runs); the udev rule gives your user access to do that.

### Module parameters

| Parameter | Default | |
|---|---|---|
| `pad` | `1` | Register the pad device (load time only) |
| `stick_dpad` | `1` | Report the A-stick as a D-pad too |
| `deadzone` | `64` | D-pad press threshold from the stick centre (128); releases at half that |
| `offscreen_edge` | `1` | Pin the pointer to 0,0 while offscreen |
| `key_resend` | `8` | Re-send the key after this many bad reports in a row (0 = never) |

Runtime-changeable ones live in `/sys/module/guncon3/parameters/`.

## Install (Arch / CachyOS)

```sh
sudo pacman -S --needed dkms linux-cachyos-headers   # headers for your kernel flavour
cd packaging/arch && makepkg -si
sudo modprobe guncon3
```

The dkms pacman hooks rebuild the module on every kernel update. Other distros:
`sudo ./install.sh` (needs `dkms` and kernel headers).

The udev rule ([70-guncon3.rules](udev/70-guncon3.rules)) hides the pointer from libinput
so the guns do not move the desktop/gamescope cursor. Remove that part of the rule if you
want to use a gun as a mouse.

## Calibration

```sh
guncon3-cal list
sudo guncon3-cal watch 3-2:1.0              # live aim: check the sensors see you everywhere
sudo guncon3-cal calibrate 3-2:1.0 --save   # port from "list"; aim at 4 corners + centre
```

The tool holds the gun open itself while it runs and works over SSH while you stand at
the screen. Stand where you will play, and keep sunlight and lamps out of the gun's view:
light reflected in the screen reads as offscreen. `--save` writes
`/etc/guncon3/<port>.conf`, re-applied whenever a gun is plugged into that port.

Calibration is `x_min x_max y_min y_max x_mid y_mid` in raw units. The centre matters:
on a real gun the corners alone left the middle of the screen ~6% off, so each half of
each axis is mapped separately. Four values (no centre) are still accepted.
The raw interface is `/sys/bus/usb/drivers/guncon3/<port>/{calibration,raw}`.

## Using it

- **RetroArch:** set the input driver to `udev`, the device type for a port to Lightgun,
  and pick the gun's pointer as that port's mouse index.
- **RPCS3:** enable USB passthrough of the GunCon 3 in the device settings; the game does
  its own calibration.

## Testing

```sh
make -C tests test                    # protocol: decoder vs upstream, field layout vs the Windows driver
make -C src && gcc -O2 -pthread -o tests/gadget/guncon3_gadget tests/gadget/guncon3_gadget.c
sudo tests/gadget/run_tests.py        # driver end to end against two emulated guns
```

`tests/gadget/guncon3_gadget` emulates a GunCon 3 with raw-gadget on dummy_hcd: it takes
the key, encrypts reports like the real gun, and can inject corrupt ones. The end-to-end
run loads `dummy_hcd`, `raw_gadget` and `src/guncon3.ko`, and covers: aim mapping, every
button on both devices, offscreen, sticks/D-pad/distance, corrupt-report recovery and key
re-send, sysfs calibration, remapping, two guns side by side, close/reopen,
unbind/rebind (the RPCS3 path) and unplug while open.

## Hardware status

Tested with two real GunCon 3s (2026-11-07):

- **Confirmed:** key exchange and decoding; endpoints (OUT 8 bytes, IN 15 bytes, 4 ms);
  all 9 buttons; both sticks (front grip = A on `ABS_X/Y`, back thumb = B on
  `ABS_RX/RY`; right = 255, up = 0); offscreen bit `0x000800`; aim and calibration;
  close/reopen; hotplug of a second gun.
- **Found and fixed on hardware:** D-pad chatter from off-axis stick drift (now with
  hysteresis), and an all-zero first report that pressed D-pad up-left for a frame.
- **Not understood yet:** status bits `0x000100` (toggles irregularly), `0x001000` and
  `0x200000`. None is needed for aim; the gun tracks fine with all three clear.
- **Still to test:** RetroArch's offscreen handling with the pointer at 0,0, and RPCS3
  passthrough detaching and re-attaching the driver.

# GunCon 3 USB Lightgun Driver

A DKMS kernel driver for the Namco GunCon 3 (`0b9a:0800`), aimed at RetroArch and
RPCS3 on Arch-based systems (developed on CachyOS, gaming mode under gamescope).
Supports two guns at once.

Fork of [pcnimdock/guncon3_dkms](https://github.com/pcnimdock/guncon3_dkms), which in
turn builds on beardypig's GunCon 2 driver and GunCon 3 protocol work.

> **Status:** tested end to end against an emulated gun (see [Testing](#testing)), not
> yet against real hardware. Items still to confirm on a real gun are listed under
> [Unverified](#unverified).

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
| `deadzone` | `20` | D-pad deadzone around the stick centre (128) |
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

Something has to have the gun's input device open (a game, or `evtest`), because the gun
only reports while it is open. Then:

```sh
guncon3-cal list
sudo guncon3-cal calibrate 3-2:1.0 --save   # port from "list"; aim at 4 corners + centre
```

The tool runs in a terminal, so it works over SSH while you stand at the screen. `--save`
writes `/etc/guncon3/<port>.conf`, re-applied whenever a gun is plugged into that port.
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

## Unverified

Decoding agrees with both the upstream driver and the Windows driver
([gameotaku79/Guncon3Windows](https://github.com/gameotaku79/Guncon3Windows)), but a real
gun still needs to confirm:

- the offscreen bit (`0x000800`, as the Windows driver uses it) and the second status bit
  (`0x001000`; upstream had `0x000100`);
- stick and Y-axis orientation, and which physical stick is "A";
- the endpoint sizes and polling interval of the real device;
- RetroArch's offscreen detection with the pointer at 0,0;
- RPCS3 passthrough detaching and re-attaching the driver.

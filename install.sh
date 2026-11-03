#!/bin/bash
# Generic DKMS install from a checkout (Ubuntu, Debian, ...).
# On Arch/CachyOS use packaging/arch/PKGBUILD instead.
set -e
VERSION=0.2.0
cd "$(dirname "$0")"
dkms add .
dkms build guncon3 -v ${VERSION}
dkms install --force guncon3 -v ${VERSION}
install -Dm644 udev/70-guncon3.rules /etc/udev/rules.d/70-guncon3.rules
install -Dm755 udev/guncon3-restore /usr/lib/guncon3/guncon3-restore
install -Dm755 tools/guncon3-cal /usr/local/bin/guncon3-cal
udevadm control --reload

#!/bin/bash
VERSION=0.2.0
rmmod guncon3 2>/dev/null
dkms remove -m guncon3 -v ${VERSION} --all
rm -f /etc/udev/rules.d/70-guncon3.rules /usr/lib/guncon3/guncon3-restore /usr/local/bin/guncon3-cal
udevadm control --reload

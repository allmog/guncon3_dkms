// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Namco GunCon 3 USB light gun
 *
 * Copyright (C) 2019-2021 beardypig <beardypig@protonmail.com>
 * Copyright (C) 2023 pcnimdock (GunCon 3 port)
 *
 * Based largely on the PXRC driver by Marcus Folkesson <marcus.folkesson@gmail.com>
 *
 * Every gun registers two input devices:
 *  - "pointer": an absolute mouse (ABS_X/ABS_Y + mouse buttons), which is
 *    what RetroArch's udev lightgun driver reads. Aim is mapped through a
 *    per-gun calibration (sysfs "calibration": edges plus the screen centre,
 *    each half of each axis linear) onto 0..65535.
 *  - "pad": every button, both sticks and the distance sensor, laid out
 *    like RPCS3's default GunCon 3 mapping.
 * Button codes on both are remappable through udev hwdb (EVIOCSKEYCODE).
 *
 * RPCS3 can pass the gun through to the emulated PS3; libusb detaches this
 * driver while it does, which ends up in guncon3_disconnect().
 */
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/usb.h>
#include <linux/usb/input.h>

#include "guncon3_proto.h"

#define GUNCON3_AXIS_MAX	65535
#define GUNCON3_KEY_TIMEOUT_MS	1000

static bool pad = true;
module_param(pad, bool, 0444);
MODULE_PARM_DESC(pad, "Register the gamepad device next to the pointer (default: on)");

static bool stick_dpad = true;
module_param(stick_dpad, bool, 0644);
MODULE_PARM_DESC(stick_dpad, "Also report the A-stick as a D-pad on the pad device (default: on)");

static uint deadzone = 64;
module_param(deadzone, uint, 0644);
MODULE_PARM_DESC(deadzone, "A-stick D-pad press threshold from the 128 centre; releases at half (default: 64)");

static bool offscreen_edge = true;
module_param(offscreen_edge, bool, 0644);
MODULE_PARM_DESC(offscreen_edge,
		 "Report the pointer at the 0,0 edge while the gun sees no screen, so emulators treat it as offscreen (default: on)");

static uint key_resend = 8;
module_param(key_resend, uint, 0644);
MODULE_PARM_DESC(key_resend, "Re-send the key after this many undecodable reports in a row, 0 = never (default: 8)");

struct guncon3_button {
	u32 mask;
	unsigned short pointer_code;	/* KEY_RESERVED = not on the pointer */
	unsigned short pad_code;
};

/* Pointer defaults follow the Windows driver's mapping.txt, pad ones RPCS3's. */
static const struct guncon3_button guncon3_buttons[] = {
	{ GUNCON3_BTN_TRIGGER, BTN_LEFT,	BTN_SOUTH },
	{ GUNCON3_BTN_A1,      BTN_RIGHT,	BTN_TL },
	{ GUNCON3_BTN_A2,      BTN_EXTRA,	BTN_TL2 },
	{ GUNCON3_BTN_A3,      KEY_RESERVED,	BTN_THUMBL },
	{ GUNCON3_BTN_B1,      BTN_SIDE,	BTN_TR },
	{ GUNCON3_BTN_B2,      BTN_MIDDLE,	BTN_TR2 },
	{ GUNCON3_BTN_B3,      KEY_RESERVED,	BTN_THUMBR },
	{ GUNCON3_BTN_C1,      KEY_RESERVED,	BTN_SELECT },
	{ GUNCON3_BTN_C2,      KEY_RESERVED,	BTN_START },
};

#define GUNCON3_NBUTTONS ARRAY_SIZE(guncon3_buttons)

/*
 * The centre is calibrated separately because a real gun's aim is not linear
 * across the screen: with the corners right, its centre landed ~6% off. Each
 * half of each axis gets its own linear map.
 */
struct guncon3_calibration {
	s32 x_min, x_max;	/* raw values that map to the left/right edge */
	s32 y_min, y_max;	/* top/bottom; min > max inverts the axis */
	s32 x_mid, y_mid;	/* raw values that map to the screen centre */
};

/* Raw Y grows upwards, screen Y downwards. */
static const struct guncon3_calibration guncon3_default_calibration = {
	.x_min = -32768, .x_max = 32767,
	.y_min = 32767, .y_max = -32768,
	.x_mid = 0, .y_mid = 0,
};

struct guncon3 {
	struct usb_interface *intf;
	struct usb_device *udev;
	struct input_dev *pointer;
	struct input_dev *pad;

	struct urb *in_urb;
	struct urb *key_urb;	/* async key re-send from the IN completion */
	u8 *in_buf;
	u8 *key_buf;		/* DMA-able copy of the key */
	unsigned int out_pipe;
	atomic_t key_busy;

	struct mutex lock;	/* open_count, start/stop, disconnected */
	unsigned int open_count;
	bool disconnected;

	spinlock_t state_lock;	/* calibration, last */
	struct guncon3_calibration calibration;
	struct guncon3_state last;
	bool have_last;

	unsigned int bad_reports;
	u32 prev_buttons;	/* IN completion only */

	unsigned short pointer_keymap[GUNCON3_NBUTTONS];
	unsigned short pad_keymap[GUNCON3_NBUTTONS];

	char pointer_phys[64];
	char pad_phys[64];
};

/* raw in [from, to] -> [out_from, out_to], clamped to that range. */
static int guncon3_lerp(s32 raw, s32 from, s32 to, int out_from, int out_to)
{
	s64 v = out_from + div_s64((s64)(raw - from) * (out_to - out_from), to - from);

	return clamp_t(s64, v, out_from, out_to);
}

/* min, mid and max are validated to be distinct and in order (either way). */
static int guncon3_map_axis(s32 raw, s32 min, s32 mid, s32 max)
{
	const int half = GUNCON3_AXIS_MAX / 2 + 1;
	bool first_half = min < max ? raw < mid : raw > mid;

	if (first_half)
		return guncon3_lerp(raw, min, mid, 0, half);
	return guncon3_lerp(raw, mid, max, half, GUNCON3_AXIS_MAX);
}

/* MSC_SCAN carries the keymap index, only on change, like HID does. */
static void guncon3_report_buttons(struct input_dev *dev, const unsigned short *keymap,
				   u32 buttons, u32 changed)
{
	unsigned int i;

	for (i = 0; i < GUNCON3_NBUTTONS; i++) {
		if (keymap[i] == KEY_RESERVED)
			continue;
		if (changed & guncon3_buttons[i].mask)
			input_event(dev, EV_MSC, MSC_SCAN, i);
		input_report_key(dev, keymap[i], buttons & guncon3_buttons[i].mask);
	}
}

/*
 * One D-pad direction from a stick offset, with hysteresis: press beyond dz,
 * release only once back inside dz/2. A held stick drifts off-axis by up to
 * ~50 on a real gun, which made a single threshold chatter.
 */
static void guncon3_dpad(struct input_dev *dev, unsigned int code, int offset, int dz)
{
	bool down = test_bit(code, dev->key) ? offset > dz / 2 : offset > dz;

	input_report_key(dev, code, down);
}

static void guncon3_report(struct guncon3 *gc, const struct guncon3_state *st)
{
	struct guncon3_calibration cal;
	bool offscreen = st->buttons & GUNCON3_STATUS_OFFSCREEN;
	u32 changed = st->buttons ^ gc->prev_buttons;
	int x = 0, y = 0;

	gc->prev_buttons = st->buttons;

	spin_lock(&gc->state_lock);
	cal = gc->calibration;
	gc->last = *st;
	gc->have_last = true;
	spin_unlock(&gc->state_lock);

	if (!(offscreen && READ_ONCE(offscreen_edge))) {
		x = guncon3_map_axis(st->x, cal.x_min, cal.x_mid, cal.x_max);
		y = guncon3_map_axis(st->y, cal.y_min, cal.y_mid, cal.y_max);
	}
	input_report_abs(gc->pointer, ABS_X, x);
	input_report_abs(gc->pointer, ABS_Y, y);
	guncon3_report_buttons(gc->pointer, gc->pointer_keymap, st->buttons, changed);
	input_sync(gc->pointer);

	if (!gc->pad)
		return;

	input_report_abs(gc->pad, ABS_X, st->ax);
	input_report_abs(gc->pad, ABS_Y, st->ay);
	input_report_abs(gc->pad, ABS_RX, st->bx);
	input_report_abs(gc->pad, ABS_RY, st->by);
	input_report_abs(gc->pad, ABS_DISTANCE, st->z);
	if (READ_ONCE(stick_dpad)) {
		int dz = clamp_t(uint, READ_ONCE(deadzone), 2, 127);

		guncon3_dpad(gc->pad, BTN_DPAD_LEFT, 128 - st->ax, dz);
		guncon3_dpad(gc->pad, BTN_DPAD_RIGHT, st->ax - 128, dz);
		guncon3_dpad(gc->pad, BTN_DPAD_UP, 128 - st->ay, dz);
		guncon3_dpad(gc->pad, BTN_DPAD_DOWN, st->ay - 128, dz);
	}
	guncon3_report_buttons(gc->pad, gc->pad_keymap, st->buttons, changed);
	input_sync(gc->pad);
}

static void guncon3_key_complete(struct urb *urb)
{
	struct guncon3 *gc = urb->context;

	if (urb->status && urb->status != -ENOENT && urb->status != -ECONNRESET &&
	    urb->status != -ESHUTDOWN)
		dev_dbg(&gc->intf->dev, "key re-send failed: %d\n", urb->status);
	atomic_set(&gc->key_busy, 0);
}

/* Called from the IN completion, so it cannot sleep. */
static void guncon3_resend_key(struct guncon3 *gc)
{
	int error;

	if (atomic_xchg(&gc->key_busy, 1))
		return;
	error = usb_submit_urb(gc->key_urb, GFP_ATOMIC);
	if (error) {
		atomic_set(&gc->key_busy, 0);
		dev_dbg(&gc->intf->dev, "could not queue key re-send: %d\n", error);
	} else {
		dev_info_ratelimited(&gc->intf->dev, "re-sending key after %u bad reports\n",
				     gc->bad_reports);
	}
}

static void guncon3_irq(struct urb *urb)
{
	struct guncon3 *gc = urb->context;
	u8 payload[GUNCON3_PAYLOAD_LEN];
	struct guncon3_state st;
	unsigned int limit;
	int error;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EPIPE:
	case -ETIME:
		dev_dbg(&gc->intf->dev, "urb stopped: %d\n", urb->status);
		return;
	default:
		dev_dbg(&gc->intf->dev, "urb status %d\n", urb->status);
		goto resubmit;
	}

	if (urb->actual_length < GUNCON3_REPORT_LEN) {
		dev_dbg(&gc->intf->dev, "short report: %d bytes\n", urb->actual_length);
		goto resubmit;
	}

	/*
	 * A report that fails the checksum is dropped, but the URB must still be
	 * resubmitted: the original driver returned here, and one corrupt report
	 * stopped input until the device was reopened.
	 */
	if (guncon3_decrypt(gc->in_buf, gc->key_buf, payload)) {
		gc->bad_reports++;
		limit = READ_ONCE(key_resend);
		if (limit && gc->bad_reports >= limit) {
			guncon3_resend_key(gc);
			gc->bad_reports = 0;
		}
		goto resubmit;
	}
	gc->bad_reports = 0;

	/*
	 * A real gun's first report after the key is all zeros, which reads as
	 * both sticks pinned up-left. No genuine report looks like that (the
	 * sticks rest at 128), so drop it.
	 */
	if (!memchr_inv(payload, 0, GUNCON3_PAYLOAD_LEN))
		goto resubmit;

	guncon3_parse(payload, &st);
	dev_dbg(&gc->intf->dev, "x=%d y=%d z=%u a=%u,%u b=%u,%u buttons=%06x\n",
		st.x, st.y, st.z, st.ax, st.ay, st.bx, st.by, st.buttons);
	guncon3_report(gc, &st);

resubmit:
	error = usb_submit_urb(urb, GFP_ATOMIC);
	if (error && error != -EPERM && error != -ENODEV)
		dev_err(&gc->intf->dev, "usb_submit_urb failed: %d\n", error);
}

/* Must hold gc->lock. Sends the key, then starts polling. */
static int guncon3_start(struct guncon3 *gc)
{
	int actual, error;

	if (gc->disconnected)
		return -ENODEV;

	gc->bad_reports = 0;
	gc->prev_buttons = 0;
	error = usb_interrupt_msg(gc->udev, gc->out_pipe, gc->key_buf, GUNCON3_KEY_LEN,
				  &actual, GUNCON3_KEY_TIMEOUT_MS);
	if (error) {
		dev_err(&gc->intf->dev, "sending key failed: %d\n", error);
		return error;
	}
	dev_dbg(&gc->intf->dev, "key sent (%d bytes)\n", actual);

	error = usb_submit_urb(gc->in_urb, GFP_KERNEL);
	if (error) {
		dev_err(&gc->intf->dev, "usb_submit_urb failed: %d\n", error);
		return -EIO;
	}
	return 0;
}

/* Must hold gc->lock. */
static void guncon3_stop(struct guncon3 *gc)
{
	dev_dbg(&gc->intf->dev, "stopping\n");
	usb_kill_urb(gc->in_urb);
	usb_kill_urb(gc->key_urb);
}

static int guncon3_open(struct input_dev *input)
{
	struct guncon3 *gc = input_get_drvdata(input);
	int error = 0;

	mutex_lock(&gc->lock);
	if (gc->open_count == 0)
		error = guncon3_start(gc);
	if (!error)
		gc->open_count++;
	mutex_unlock(&gc->lock);
	return error;
}

static void guncon3_close(struct input_dev *input)
{
	struct guncon3 *gc = input_get_drvdata(input);

	mutex_lock(&gc->lock);
	if (gc->open_count && --gc->open_count == 0)
		guncon3_stop(gc);
	mutex_unlock(&gc->lock);
}

/* sysfs: /sys/bus/usb/drivers/guncon3/<interface>/{calibration,raw} */

static ssize_t calibration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct guncon3 *gc = usb_get_intfdata(to_usb_interface(dev));
	struct guncon3_calibration cal;

	spin_lock_irq(&gc->state_lock);
	cal = gc->calibration;
	spin_unlock_irq(&gc->state_lock);

	return sysfs_emit(buf, "%d %d %d %d %d %d\n", cal.x_min, cal.x_max, cal.y_min, cal.y_max,
			  cal.x_mid, cal.y_mid);
}

static bool guncon3_cal_axis_ok(s32 min, s32 mid, s32 max)
{
	return min < max ? min < mid && mid < max : max < mid && mid < min;
}

static bool guncon3_cal_in_range(s32 v)
{
	return v >= S16_MIN * 2 && v <= S16_MAX * 2;
}

/*
 * Accepts "x_min x_max y_min y_max [x_mid y_mid]" in raw units, or "reset".
 * Without a centre, the midpoints are used (a plain linear map).
 */
static ssize_t calibration_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct guncon3 *gc = usb_get_intfdata(to_usb_interface(dev));
	struct guncon3_calibration cal;

	if (sysfs_streq(buf, "reset")) {
		cal = guncon3_default_calibration;
	} else {
		int n = sscanf(buf, "%d %d %d %d %d %d", &cal.x_min, &cal.x_max,
			       &cal.y_min, &cal.y_max, &cal.x_mid, &cal.y_mid);

		if (n != 4 && n != 6)
			return -EINVAL;
		if (!guncon3_cal_in_range(cal.x_min) || !guncon3_cal_in_range(cal.x_max) ||
		    !guncon3_cal_in_range(cal.y_min) || !guncon3_cal_in_range(cal.y_max))
			return -ERANGE;
		if (n == 4) {
			cal.x_mid = cal.x_min + (cal.x_max - cal.x_min) / 2;
			cal.y_mid = cal.y_min + (cal.y_max - cal.y_min) / 2;
		}
		if (!guncon3_cal_axis_ok(cal.x_min, cal.x_mid, cal.x_max) ||
		    !guncon3_cal_axis_ok(cal.y_min, cal.y_mid, cal.y_max))
			return -EINVAL;
	}

	spin_lock_irq(&gc->state_lock);
	gc->calibration = cal;
	spin_unlock_irq(&gc->state_lock);
	return count;
}
static DEVICE_ATTR_RW(calibration);

/* Last decoded report, uncalibrated, for calibration tools: "x y z buttons". */
static ssize_t raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct guncon3 *gc = usb_get_intfdata(to_usb_interface(dev));
	struct guncon3_state st;
	bool have;

	spin_lock_irq(&gc->state_lock);
	st = gc->last;
	have = gc->have_last;
	spin_unlock_irq(&gc->state_lock);

	if (!have)
		return -ENODATA;
	return sysfs_emit(buf, "%d %d %u %06x\n", st.x, st.y, st.z, st.buttons);
}
static DEVICE_ATTR_RO(raw);

static struct attribute *guncon3_attrs[] = {
	&dev_attr_calibration.attr,
	&dev_attr_raw.attr,
	NULL,
};
ATTRIBUTE_GROUPS(guncon3);

static void guncon3_setup_keymap(struct input_dev *dev, unsigned short *keymap, bool for_pad)
{
	unsigned int i;

	for (i = 0; i < GUNCON3_NBUTTONS; i++) {
		keymap[i] = for_pad ? guncon3_buttons[i].pad_code : guncon3_buttons[i].pointer_code;
		if (keymap[i] != KEY_RESERVED)
			input_set_capability(dev, EV_KEY, keymap[i]);
	}
	dev->keycode = keymap;
	dev->keycodesize = sizeof(*keymap);
	dev->keycodemax = GUNCON3_NBUTTONS;
	input_set_capability(dev, EV_MSC, MSC_SCAN);
}

static struct input_dev *guncon3_alloc_input(struct guncon3 *gc, const char *name,
					     char *phys, size_t phys_len, int index)
{
	struct input_dev *dev;

	dev = devm_input_allocate_device(&gc->intf->dev);
	if (!dev)
		return NULL;

	usb_make_path(gc->udev, phys, phys_len);
	snprintf(phys + strlen(phys), phys_len - strlen(phys), "/input%d", index);

	dev->name = name;
	dev->phys = phys;
	usb_to_input_id(gc->udev, &dev->id);
	dev->dev.parent = &gc->intf->dev;
	dev->open = guncon3_open;
	dev->close = guncon3_close;
	input_set_drvdata(dev, gc);
	return dev;
}

static void guncon3_free_urbs(void *data)
{
	struct guncon3 *gc = data;

	usb_kill_urb(gc->in_urb);
	usb_kill_urb(gc->key_urb);
	usb_free_urb(gc->in_urb);
	usb_free_urb(gc->key_urb);
}

static int guncon3_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *ep_in, *ep_out;
	struct guncon3 *gc;
	size_t in_len;
	int error;

	error = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL, &ep_in, &ep_out);
	if (error) {
		dev_err(&intf->dev, "interrupt IN/OUT endpoints not found\n");
		return error;
	}

	in_len = usb_endpoint_maxp(ep_in);
	if (in_len < GUNCON3_REPORT_LEN) {
		dev_err(&intf->dev, "IN endpoint too small: %zu bytes\n", in_len);
		return -ENODEV;
	}

	gc = devm_kzalloc(&intf->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	gc->intf = intf;
	gc->udev = udev;
	gc->calibration = guncon3_default_calibration;
	mutex_init(&gc->lock);
	spin_lock_init(&gc->state_lock);
	atomic_set(&gc->key_busy, 0);
	gc->out_pipe = usb_sndintpipe(udev, ep_out->bEndpointAddress);

	gc->in_buf = devm_kzalloc(&intf->dev, in_len, GFP_KERNEL);
	gc->key_buf = devm_kmemdup(&intf->dev, guncon3_default_key, GUNCON3_KEY_LEN, GFP_KERNEL);
	if (!gc->in_buf || !gc->key_buf)
		return -ENOMEM;

	gc->in_urb = usb_alloc_urb(0, GFP_KERNEL);
	gc->key_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!gc->in_urb || !gc->key_urb) {
		usb_free_urb(gc->in_urb);
		usb_free_urb(gc->key_urb);
		return -ENOMEM;
	}
	error = devm_add_action_or_reset(&intf->dev, guncon3_free_urbs, gc);
	if (error)
		return error;

	usb_fill_int_urb(gc->in_urb, udev, usb_rcvintpipe(udev, ep_in->bEndpointAddress),
			 gc->in_buf, in_len, guncon3_irq, gc, ep_in->bInterval);
	usb_fill_int_urb(gc->key_urb, udev, gc->out_pipe, gc->key_buf, GUNCON3_KEY_LEN,
			 guncon3_key_complete, gc, ep_out->bInterval);

	usb_set_intfdata(intf, gc);

	gc->pointer = guncon3_alloc_input(gc, "Namco GunCon 3 Pointer", gc->pointer_phys,
					  sizeof(gc->pointer_phys), 0);
	if (!gc->pointer)
		return -ENOMEM;
	input_set_abs_params(gc->pointer, ABS_X, 0, GUNCON3_AXIS_MAX, 0, 0);
	input_set_abs_params(gc->pointer, ABS_Y, 0, GUNCON3_AXIS_MAX, 0, 0);
	guncon3_setup_keymap(gc->pointer, gc->pointer_keymap, false);

	if (pad) {
		gc->pad = guncon3_alloc_input(gc, "Namco GunCon 3 Pad", gc->pad_phys,
					      sizeof(gc->pad_phys), 1);
		if (!gc->pad)
			return -ENOMEM;
		input_set_abs_params(gc->pad, ABS_X, 0, 255, 0, 8);
		input_set_abs_params(gc->pad, ABS_Y, 0, 255, 0, 8);
		input_set_abs_params(gc->pad, ABS_RX, 0, 255, 0, 8);
		input_set_abs_params(gc->pad, ABS_RY, 0, 255, 0, 8);
		input_set_abs_params(gc->pad, ABS_DISTANCE, 0, U16_MAX, 0, 0);
		/* Always advertised: stick_dpad can be switched on at runtime. */
		input_set_capability(gc->pad, EV_KEY, BTN_DPAD_UP);
		input_set_capability(gc->pad, EV_KEY, BTN_DPAD_DOWN);
		input_set_capability(gc->pad, EV_KEY, BTN_DPAD_LEFT);
		input_set_capability(gc->pad, EV_KEY, BTN_DPAD_RIGHT);
		guncon3_setup_keymap(gc->pad, gc->pad_keymap, true);
	}

	error = input_register_device(gc->pointer);
	if (error)
		return error;
	if (gc->pad) {
		error = input_register_device(gc->pad);
		if (error)
			return error;
	}
	return 0;
}

static void guncon3_disconnect(struct usb_interface *intf)
{
	struct guncon3 *gc = usb_get_intfdata(intf);

	/* Input devices, URBs and buffers are devm-managed and go after this. */
	mutex_lock(&gc->lock);
	gc->disconnected = true;
	guncon3_stop(gc);
	mutex_unlock(&gc->lock);
}

static int guncon3_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct guncon3 *gc = usb_get_intfdata(intf);

	mutex_lock(&gc->lock);
	if (gc->open_count)
		guncon3_stop(gc);
	mutex_unlock(&gc->lock);
	return 0;
}

static int guncon3_resume(struct usb_interface *intf)
{
	struct guncon3 *gc = usb_get_intfdata(intf);
	int error = 0;

	/* The gun may have lost power, so it needs the key again. */
	mutex_lock(&gc->lock);
	if (gc->open_count)
		error = guncon3_start(gc);
	mutex_unlock(&gc->lock);
	return error;
}

static int guncon3_pre_reset(struct usb_interface *intf)
{
	struct guncon3 *gc = usb_get_intfdata(intf);

	mutex_lock(&gc->lock);
	guncon3_stop(gc);
	return 0;
}

static int guncon3_post_reset(struct usb_interface *intf)
{
	struct guncon3 *gc = usb_get_intfdata(intf);
	int error = 0;

	if (gc->open_count)
		error = guncon3_start(gc);
	mutex_unlock(&gc->lock);
	return error;
}

static const struct usb_device_id guncon3_table[] = {
	{ USB_DEVICE(GUNCON3_VENDOR_ID, GUNCON3_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, guncon3_table);

static struct usb_driver guncon3_driver = {
	.name = "guncon3",
	.probe = guncon3_probe,
	.disconnect = guncon3_disconnect,
	.id_table = guncon3_table,
	.suspend = guncon3_suspend,
	.resume = guncon3_resume,
	.reset_resume = guncon3_resume,
	.pre_reset = guncon3_pre_reset,
	.post_reset = guncon3_post_reset,
	.dev_groups = guncon3_groups,
};
module_usb_driver(guncon3_driver);

MODULE_AUTHOR("beardypig <beardypig@protonmail.com>");
MODULE_AUTHOR("pcnimdock");
MODULE_DESCRIPTION("Namco GunCon 3 USB light gun driver");
MODULE_LICENSE("GPL");

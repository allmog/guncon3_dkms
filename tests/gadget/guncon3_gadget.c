// SPDX-License-Identifier: GPL-2.0
/*
 * Software GunCon 3 for testing the driver without a gun.
 *
 * Presents 0b9a:0800 through raw-gadget on a dummy_hcd UDC, takes the key
 * on the interrupt OUT endpoint like the real gun, and streams encrypted
 * reports on interrupt IN. State is driven by lines on stdin:
 *
 *   state <x> <y> <z> <buttons-hex> <ax> <ay> <bx> <by>
 *   corrupt <n>     send n reports that fail the checksum
 *   pause | resume  stop/restart sending reports
 *   quit
 *
 * Events go to stdout as lines: "READY", "KEY <n> <hex>", "SENT <n>".
 *
 * Needs root plus: modprobe dummy_hcd num=<guns>; modprobe raw_gadget
 * Usage: guncon3_gadget [udc-device]   (default dummy_udc.0)
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>

#include "../guncon3_encrypt.h"

#define EP0_MAX		64
#define IN_MAXP		16
#define OUT_MAXP	8
#define INTERVAL	4

struct io_buf {
	struct usb_raw_ep_io io;
	unsigned char data[256];
};

static int fd;
static int ep_in = -1, ep_out = -1;
static unsigned char addr_in, addr_out;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct guncon3_state state = {
	.ax = 128, .ay = 128, .bx = 128, .by = 128,
};
static unsigned char key[GUNCON3_KEY_LEN];
static bool keyed, paused, configured;
static int corrupt_left;
static unsigned long keys_seen, reports_sent;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	pthread_mutex_unlock(&lock);
}

static void die(const char *what)
{
	fprintf(stderr, "guncon3_gadget: %s: %s\n", what, strerror(errno));
	exit(1);
}

static struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bMaxPacketSize0 = EP0_MAX,
	.idVendor = GUNCON3_VENDOR_ID,
	.idProduct = GUNCON3_PRODUCT_ID,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

static int build_config(unsigned char *buf)
{
	struct usb_config_descriptor *cfg = (void *)buf;
	struct usb_interface_descriptor *intf = (void *)(buf + USB_DT_CONFIG_SIZE);
	struct usb_endpoint_descriptor *in = (void *)(buf + USB_DT_CONFIG_SIZE + USB_DT_INTERFACE_SIZE);
	struct usb_endpoint_descriptor *out = (void *)((unsigned char *)in + USB_DT_ENDPOINT_SIZE);
	int total = USB_DT_CONFIG_SIZE + USB_DT_INTERFACE_SIZE + 2 * USB_DT_ENDPOINT_SIZE;

	memset(buf, 0, total);
	*cfg = (struct usb_config_descriptor) {
		.bLength = USB_DT_CONFIG_SIZE, .bDescriptorType = USB_DT_CONFIG,
		.wTotalLength = total, .bNumInterfaces = 1, .bConfigurationValue = 1,
		.bmAttributes = USB_CONFIG_ATT_ONE, .bMaxPower = 50,
	};
	*intf = (struct usb_interface_descriptor) {
		.bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
		.bNumEndpoints = 2, .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
	};
	in->bLength = USB_DT_ENDPOINT_SIZE;
	in->bDescriptorType = USB_DT_ENDPOINT;
	in->bEndpointAddress = USB_DIR_IN | addr_in;
	in->bmAttributes = USB_ENDPOINT_XFER_INT;
	in->wMaxPacketSize = IN_MAXP;
	in->bInterval = INTERVAL;
	out->bLength = USB_DT_ENDPOINT_SIZE;
	out->bDescriptorType = USB_DT_ENDPOINT;
	out->bEndpointAddress = USB_DIR_OUT | addr_out;
	out->bmAttributes = USB_ENDPOINT_XFER_INT;
	out->wMaxPacketSize = OUT_MAXP;
	out->bInterval = INTERVAL;
	return total;
}

static int build_string(int index, unsigned char *buf)
{
	const char *s = index == 1 ? "Namco" : "GunCon3 (emulated)";
	int i, n = strlen(s);

	if (index == 0) {
		buf[0] = 4; buf[1] = USB_DT_STRING; buf[2] = 0x09; buf[3] = 0x04;
		return 4;
	}
	buf[0] = 2 + 2 * n;
	buf[1] = USB_DT_STRING;
	for (i = 0; i < n; i++) {
		buf[2 + 2 * i] = s[i];
		buf[3 + 2 * i] = 0;
	}
	return buf[0];
}

/* Pick UDC endpoints that can do interrupt transfers in each direction. */
static void pick_endpoints(void)
{
	struct usb_raw_eps_info info;
	int i, n;

	memset(&info, 0, sizeof(info));
	n = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, &info);
	if (n < 0)
		die("EPS_INFO");
	/*
	 * dummy_udc has fixed-address interrupt IN endpoints but interrupt OUT
	 * only on "any address" ones; those take whatever number we put in the
	 * descriptor. 2 is the OUT endpoint upstream hard-coded for the real gun.
	 */
	for (i = 0; i < n; i++) {
		struct usb_raw_ep_info *e = &info.eps[i];

		if (!e->caps.type_int)
			continue;
		if (!addr_in && e->caps.dir_in && e->addr != USB_RAW_EP_ADDR_ANY)
			addr_in = e->addr;
		else if (!addr_out && e->caps.dir_out)
			addr_out = e->addr == USB_RAW_EP_ADDR_ANY ? 2 : e->addr;
	}
	if (addr_in == addr_out)
		addr_out = 0;
	if (!addr_in || !addr_out) {
		fprintf(stderr, "guncon3_gadget: UDC has no interrupt IN/OUT pair:\n");
		for (i = 0; i < n; i++)
			fprintf(stderr, "  %s addr=%u int=%u in=%u out=%u\n", info.eps[i].name,
				info.eps[i].addr, info.eps[i].caps.type_int,
				info.eps[i].caps.dir_in, info.eps[i].caps.dir_out);
		exit(1);
	}
}

static int enable_ep(unsigned char address)
{
	struct usb_endpoint_descriptor d = {
		.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = address, .bmAttributes = USB_ENDPOINT_XFER_INT,
		.wMaxPacketSize = (address & USB_DIR_IN) ? IN_MAXP : OUT_MAXP,
		.bInterval = INTERVAL,
	};
	int ep = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, &d);

	if (ep < 0)
		die("EP_ENABLE");
	return ep;
}

static void *out_thread(void *arg)
{
	struct io_buf b;

	for (;;) {
		int i, n;
		char hex[3 * GUNCON3_KEY_LEN + 1] = "";

		b.io.ep = ep_out;
		b.io.flags = 0;
		b.io.length = OUT_MAXP;
		n = ioctl(fd, USB_RAW_IOCTL_EP_READ, &b.io);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ESHUTDOWN || errno == EBUSY)
				return NULL;
			die("EP_READ");
		}
		if (n != GUNCON3_KEY_LEN || b.io.data[0] != 0x01)
			continue;	/* the real gun ignores anything that is not a key */

		pthread_mutex_lock(&lock);
		memcpy(key, b.io.data, GUNCON3_KEY_LEN);
		keyed = true;
		keys_seen++;
		for (i = 0; i < GUNCON3_KEY_LEN; i++)
			sprintf(hex + 2 * i, "%02x", key[i]);
		pthread_mutex_unlock(&lock);
		emit("KEY %lu %s", keys_seen, hex);
	}
}

static void *in_thread(void *arg)
{
	struct io_buf b;
	unsigned char seed = 0;

	for (;;) {
		unsigned char payload[GUNCON3_PAYLOAD_LEN];
		bool send, corrupt = false;

		pthread_mutex_lock(&lock);
		send = keyed && !paused;
		if (send) {
			guncon3_unparse(&state, payload);
			guncon3_encrypt(payload, key, seed++, b.io.data);
			if (corrupt_left > 0) {
				corrupt_left--;
				corrupt = true;
			}
		}
		pthread_mutex_unlock(&lock);

		if (!send) {
			usleep(1000);
			continue;
		}
		if (corrupt)
			b.io.data[0] ^= 0x5a;	/* any change to byte 0 breaks the checksum */

		b.io.ep = ep_in;
		b.io.flags = 0;
		b.io.length = GUNCON3_REPORT_LEN;
		if (ioctl(fd, USB_RAW_IOCTL_EP_WRITE, &b.io) < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ESHUTDOWN || errno == EBUSY)
				return NULL;
			die("EP_WRITE");
		}
		pthread_mutex_lock(&lock);
		reports_sent++;
		pthread_mutex_unlock(&lock);
	}
}

static void *stdin_thread(void *arg)
{
	char line[256];

	while (fgets(line, sizeof(line), stdin)) {
		struct guncon3_state s;
		int x, y, z, ax, ay, bx, by, n;
		unsigned int buttons;

		if (sscanf(line, "state %d %d %d %x %d %d %d %d",
			   &x, &y, &z, &buttons, &ax, &ay, &bx, &by) == 8) {
			s = (struct guncon3_state) {
				.buttons = buttons, .x = x, .y = y, .z = z,
				.ax = ax, .ay = ay, .bx = bx, .by = by,
			};
			pthread_mutex_lock(&lock);
			state = s;
			pthread_mutex_unlock(&lock);
		} else if (sscanf(line, "corrupt %d", &n) == 1) {
			pthread_mutex_lock(&lock);
			corrupt_left += n;
			pthread_mutex_unlock(&lock);
		} else if (!strncmp(line, "pause", 5)) {
			pthread_mutex_lock(&lock);
			paused = true;
			pthread_mutex_unlock(&lock);
		} else if (!strncmp(line, "resume", 6)) {
			pthread_mutex_lock(&lock);
			paused = false;
			pthread_mutex_unlock(&lock);
		} else if (!strncmp(line, "sent", 4)) {
			unsigned long n_sent;

			pthread_mutex_lock(&lock);
			n_sent = reports_sent;
			pthread_mutex_unlock(&lock);
			emit("SENT %lu", n_sent);
		} else if (!strncmp(line, "quit", 4)) {
			break;
		}
	}
	exit(0);
}

static void ep0_reply(const void *data, int len, int wlength)
{
	struct io_buf b;

	b.io.ep = 0;
	b.io.flags = 0;
	b.io.length = len < wlength ? len : wlength;
	memcpy(b.io.data, data, b.io.length);
	if (ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, &b.io) < 0)
		die("EP0_WRITE");
}

static void ep0_ack(void)
{
	struct io_buf b;

	b.io.ep = 0;
	b.io.flags = 0;
	b.io.length = 0;
	if (ioctl(fd, USB_RAW_IOCTL_EP0_READ, &b.io) < 0)
		die("EP0_READ");
}

static void ep0_stall(void)
{
	if (ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0) < 0)
		die("EP0_STALL");
}

static void handle_control(const struct usb_ctrlrequest *ctrl)
{
	unsigned char buf[256];
	int type = ctrl->wValue >> 8, len;
	pthread_t t;

	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
		ep0_stall();
		return;
	}

	switch (ctrl->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		if (type == USB_DT_DEVICE) {
			ep0_reply(&dev_desc, sizeof(dev_desc), ctrl->wLength);
		} else if (type == USB_DT_CONFIG) {
			len = build_config(buf);
			ep0_reply(buf, len, ctrl->wLength);
		} else if (type == USB_DT_STRING && (ctrl->wValue & 0xff) <= 2) {
			len = build_string(ctrl->wValue & 0xff, buf);
			ep0_reply(buf, len, ctrl->wLength);
		} else {
			ep0_stall();
		}
		return;
	case USB_REQ_SET_CONFIGURATION:
		if (!configured) {
			ep_in = enable_ep(USB_DIR_IN | addr_in);
			ep_out = enable_ep(USB_DIR_OUT | addr_out);
			if (ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, 100) < 0)
				die("VBUS_DRAW");
			if (ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0) < 0)
				die("CONFIGURE");
			pthread_create(&t, NULL, out_thread, NULL);
			pthread_create(&t, NULL, in_thread, NULL);
			configured = true;
		}
		ep0_ack();
		emit("READY");
		return;
	case USB_REQ_GET_STATUS:
		buf[0] = 0;
		buf[1] = 0;
		ep0_reply(buf, 2, ctrl->wLength);
		return;
	case USB_REQ_GET_CONFIGURATION:
		buf[0] = configured;
		ep0_reply(buf, 1, ctrl->wLength);
		return;
	case USB_REQ_GET_INTERFACE:
		buf[0] = 0;
		ep0_reply(buf, 1, ctrl->wLength);
		return;
	default:
		/* SET_INTERFACE, CLEAR_FEATURE, ...: nothing to do */
		if (ctrl->bRequestType & USB_DIR_IN)
			ep0_stall();
		else
			ep0_ack();
		return;
	}
}

int main(int argc, char **argv)
{
	const char *udc = argc > 1 ? argv[1] : "dummy_udc.0";
	struct usb_raw_init init = { .speed = USB_SPEED_FULL };
	struct {
		struct usb_raw_event ev;
		unsigned char data[sizeof(struct usb_ctrlrequest)];
	} e;
	pthread_t t;

	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0)
		die("open /dev/raw-gadget (modprobe raw_gadget?)");

	snprintf((char *)init.driver_name, UDC_NAME_LENGTH_MAX, "dummy_udc");
	snprintf((char *)init.device_name, UDC_NAME_LENGTH_MAX, "%s", udc);
	if (ioctl(fd, USB_RAW_IOCTL_INIT, &init) < 0)
		die("INIT");
	if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0)
		die("RUN");

	pthread_create(&t, NULL, stdin_thread, NULL);

	for (;;) {
		e.ev.type = 0;
		e.ev.length = sizeof(e.data);
		if (ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, &e.ev) < 0) {
			if (errno == EINTR)
				continue;
			die("EVENT_FETCH");
		}
		if (e.ev.type == USB_RAW_EVENT_CONNECT)
			pick_endpoints();
		else if (e.ev.type == USB_RAW_EVENT_CONTROL)
			handle_control((struct usb_ctrlrequest *)e.ev.data);
	}
}

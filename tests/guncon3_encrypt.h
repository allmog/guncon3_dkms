/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Inverse of guncon3_decrypt(): builds the 15-byte report a real gun would
 * send for a given payload. Test/emulator use only; the driver never needs it.
 */
#ifndef GUNCON3_ENCRYPT_H
#define GUNCON3_ENCRYPT_H

#include "../src/guncon3_proto.h"

static inline int guncon3_checksum(const gc3_u8 *raw)
{
	int a_sum, b_sum;

	b_sum = ((raw[13] ^ raw[12]) + raw[11] + raw[10] - raw[9] - raw[8]) ^ raw[7];
	b_sum &= 0xFF;
	a_sum = (((raw[6] ^ b_sum) - raw[5] - raw[4]) ^ raw[3]) + raw[2] + raw[1] - raw[0];
	return a_sum & 0xFF;
}

/*
 * @seed becomes raw[14], which picks where in the key table decryption starts;
 * a real gun varies it per report.
 */
static inline void guncon3_encrypt(const gc3_u8 *payload, const gc3_u8 *key,
				   gc3_u8 seed, gc3_u8 *raw)
{
	gc3_u8 bkeys[GUNCON3_PAYLOAD_LEN][3], keyrs[GUNCON3_PAYLOAD_LEN][3];
	int key_offset, key_index, x, y, pad;

	raw[14] = seed;
	key_offset = (((((key[1] ^ key[2]) - key[3] - key[4]) ^ key[5]) + key[6] - key[7])
		      ^ raw[14]) + 0x26;
	key_offset &= 0xFF;
	key_index = 4;

	/* Replay the decryption's key schedule... */
	for (x = 12; x >= 0; x--) {
		for (y = 0; y < 3; y++) {
			key_offset--;
			bkeys[x][y] = guncon3_key_table[key_offset + 0x41];
			keyrs[x][y] = key[key_index];
			if (--key_index == 0)
				key_index = 7;
		}
	}

	/* ...and undo its three steps per byte, last one first. */
	for (x = 0; x < GUNCON3_PAYLOAD_LEN; x++) {
		gc3_u8 byte = payload[x];

		for (y = 2; y >= 0; y--) {
			switch (bkeys[x][y] & 3) {
			case 0:
				byte = byte + bkeys[x][y] + keyrs[x][y];
				break;
			case 1:
				byte = byte - bkeys[x][y] - keyrs[x][y];
				break;
			default:
				byte = byte ^ bkeys[x][y] ^ keyrs[x][y];
				break;
			}
		}
		raw[x] = byte;
	}

	/* raw[13] is padding; exactly one value makes the checksum match. */
	for (pad = 0; pad < 256; pad++) {
		raw[13] = (gc3_u8)pad;
		if (guncon3_checksum(raw) == key[7])
			return;
	}
}

/* Inverse of guncon3_parse(). */
static inline void guncon3_unparse(const struct guncon3_state *st, gc3_u8 *p)
{
	p[0] = st->buttons & 0xFF;
	p[1] = (st->buttons >> 8) & 0xFF;
	p[2] = (st->buttons >> 16) & 0xFF;
	p[3] = (gc3_u16)st->x & 0xFF;
	p[4] = (gc3_u16)st->x >> 8;
	p[5] = (gc3_u16)st->y & 0xFF;
	p[6] = (gc3_u16)st->y >> 8;
	p[7] = st->z & 0xFF;
	p[8] = st->z >> 8;
	p[9] = st->ax;
	p[10] = st->ay;
	p[11] = st->bx;
	p[12] = st->by;
}

#endif /* GUNCON3_ENCRYPT_H */

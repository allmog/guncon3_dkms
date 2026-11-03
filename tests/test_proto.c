// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace checks for src/guncon3_proto.h, runnable without a gun:
 *  - the key table matches upstream byte for byte;
 *  - guncon3_decrypt() matches upstream's guncon3_decode() on random
 *    reports, both with valid and invalid checksums;
 *  - guncon3_parse() puts every field where the Windows driver
 *    (gameotaku79/Guncon3Windows GunconReader.cs) reads it;
 *  - guncon3_encrypt() (used by the gadget emulator) round-trips through
 *    both our decrypt and upstream's, for random keys and payloads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ref_decode.h"
#include "../src/guncon3_proto.h"
#include "guncon3_encrypt.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
} while (0)

/* Make raw[0] satisfy the checksum for @k, so the decrypt path runs. */
static void fix_checksum(unsigned char *raw, const unsigned char *k)
{
	int b_sum, partial;

	b_sum = ((raw[13] ^ raw[12]) + raw[11] + raw[10] - raw[9] - raw[8]) ^ raw[7];
	b_sum &= 0xFF;
	partial = (((raw[6] ^ b_sum) - raw[5] - raw[4]) ^ raw[3]) + raw[2] + raw[1];
	raw[0] = (unsigned char)(partial - k[7]);
}

static void test_tables(void)
{
	CHECK(memcmp(KEY_TABLE, guncon3_key_table, sizeof(KEY_TABLE)) == 0,
	      "key table differs from upstream");
	CHECK(memcmp(key, guncon3_default_key, sizeof(key)) == 0,
	      "default key differs from upstream");
}

static void test_decrypt_matches_reference(void)
{
	int i, n_valid = 0;

	srand(1);
	for (i = 0; i < 200000; i++) {
		unsigned char raw[15], ref_in[15], ref_out[15] = {0};
		unsigned char out[13] = {0};
		int j, ref_rc, rc;

		for (j = 0; j < 15; j++)
			raw[j] = rand() & 0xFF;
		if (i % 4 != 0)
			fix_checksum(raw, key);

		memcpy(ref_in, raw, 15);
		ref_rc = guncon3_decode(ref_in, ref_out);
		rc = guncon3_decrypt(raw, guncon3_default_key, out);

		CHECK(ref_rc == rc, "iteration %d: rc %d, reference %d", i, rc, ref_rc);
		if (rc == 0) {
			n_valid++;
			CHECK(memcmp(out, ref_out, 13) == 0, "iteration %d: payload differs", i);
		}
	}
	CHECK(n_valid > 140000, "only %d reports took the decrypt path", n_valid);
}

/* Field offsets as GunconReader.cs reads them; decoded[i] == payload[12 - i]. */
static void test_parse_matches_windows(void)
{
	unsigned char p[13];
	unsigned char d[13];
	struct guncon3_state st;
	int i, bit;

	for (i = 0; i < 13; i++)
		p[i] = (unsigned char)(0x11 * (i + 1));
	for (i = 0; i < 13; i++)
		d[i] = p[12 - i];
	guncon3_parse(p, &st);

	CHECK(st.x == (short)(d[8] * 256 + d[9]), "X");
	CHECK(st.y == (short)(d[6] * 256 + d[7]), "Y");
	CHECK(st.z == (unsigned short)(d[4] * 256 + d[5]), "Z");
	CHECK(st.ax == d[3] && st.ay == d[2], "A-stick (Windows HAT0X/HAT0Y)");
	CHECK(st.bx == d[1] && st.by == d[0], "B-stick (Windows RX/RY)");

	/* Each button, alone, must map to exactly the Windows bit. */
	for (bit = 0; bit < 24; bit++) {
		struct { unsigned int mask; int byte; int winbit; const char *name; } map[] = {
			{ GUNCON3_BTN_TRIGGER, 11, 0x20, "Trigger" },
			{ GUNCON3_BTN_A1, 12, 0x04, "A1" },
			{ GUNCON3_BTN_A2, 12, 0x02, "A2" },
			{ GUNCON3_BTN_B1, 11, 0x04, "B1" },
			{ GUNCON3_BTN_B2, 11, 0x02, "B2" },
			{ GUNCON3_BTN_C1, 11, 0x80, "C1" },
			{ GUNCON3_BTN_C2, 12, 0x08, "C2" },
			{ GUNCON3_BTN_A3, 10, 0x80, "AClick" },
			{ GUNCON3_BTN_B3, 10, 0x40, "BClick" },
			{ GUNCON3_STATUS_OFFSCREEN, 11, 0x08, "INDICATOR2 (offscreen)" },
			{ GUNCON3_STATUS_INDICATOR, 11, 0x10, "INDICATOR1" },
		};
		unsigned int k;

		memset(p, 0, sizeof(p));
		p[bit / 8] = (unsigned char)(1u << (bit % 8));
		for (i = 0; i < 13; i++)
			d[i] = p[12 - i];
		guncon3_parse(p, &st);

		for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
			int ours = (st.buttons & map[k].mask) != 0;
			int theirs = (d[map[k].byte] & map[k].winbit) != 0;

			CHECK(ours == theirs, "%s disagrees with Windows for payload bit %d",
			      map[k].name, bit);
		}
	}
}

static void test_encrypt_roundtrip(void)
{
	int i, j;

	srand(2);
	for (i = 0; i < 100000; i++) {
		unsigned char k[8], payload[13], raw[15], out[13], ref_out[15];
		struct guncon3_state st, back;

		k[0] = 0x01;
		for (j = 1; j < 8; j++)
			k[j] = (i == 0) ? guncon3_default_key[j] : rand() & 0xFF;
		for (j = 0; j < 13; j++)
			payload[j] = rand() & 0xFF;

		guncon3_encrypt(payload, k, rand() & 0xFF, raw);
		CHECK(guncon3_decrypt(raw, k, out) == 0, "iteration %d: checksum rejected", i);
		CHECK(memcmp(out, payload, 13) == 0, "iteration %d: round trip differs", i);

		if (memcmp(k, key, 8) == 0) {
			CHECK(guncon3_decode(raw, ref_out) == 0 && memcmp(ref_out, payload, 13) == 0,
			      "iteration %d: upstream decoder disagrees", i);
		}

		guncon3_parse(payload, &st);
		guncon3_unparse(&st, out);
		guncon3_parse(out, &back);
		CHECK(memcmp(out, payload, 13) == 0, "iteration %d: unparse differs", i);
	}
}

int main(void)
{
	test_tables();
	test_decrypt_matches_reference();
	test_parse_matches_windows();
	test_encrypt_roundtrip();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all protocol checks passed\n");
	return 0;
}

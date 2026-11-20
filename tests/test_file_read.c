/*
    Offline test for nspire_file_read: the USB transport is stubbed, so no
    calculator is needed. The fake device sends DEV_LEN bytes; the caller's
    buffer is smaller. nspire_file_read must not write past `size`.

    ./configure && cc -I. -Isrc -Isrc/api $(pkg-config --cflags libusb-1.0) -fsanitize=address -g \
       tests/test_file_read.c src/services/file.c -o /tmp/test_file_read && /tmp/test_file_read
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "handle.h"
#include "error.h"
#include "data.h"
#include "service.h"
#include "services/file.h"

#define DEV_LEN 1000		/* file size the fake device sends */
#define MAXDATA 254		/* packet_max_datasize */

static uint32_t remaining;	/* bytes the fake device has yet to send */
static int first_read, sent_ff00, empty_chunk;

uint32_t packet_max_datasize(nspire_handle_t *h) { (void)h; return MAXDATA; }
int service_connect(nspire_handle_t *h, uint16_t sid) { (void)h; (void)sid; first_read = 1; return 0; }
int service_disconnect(nspire_handle_t *h) { (void)h; return 0; }
int data_build(const char *f, void *p, size_t m, size_t *len, ...) { (void)f; (void)p; (void)m; *len = 1; return 0; }

int data_write(nspire_handle_t *h, void *p, size_t len) {
	(void)h;
	if (len == 2 && ((uint8_t *)p)[0] == 0xFF) sent_ff00 = 1;
	return 0;
}

int data_scan(const char *f, const void *p, size_t m, ...) {
	(void)f; (void)p; (void)m;
	va_list ap; va_start(ap, m);
	*va_arg(ap, uint16_t *) = 0x0301;	/* file follows */
	*va_arg(ap, uint32_t *) = DEV_LEN;
	va_end(ap);
	return 0;
}

int data_read(nspire_handle_t *h, void *p, size_t maxlen, size_t *actual) {
	(void)h;
	if (first_read) { first_read = 0; if (actual) *actual = maxlen; return 0; }
	if (empty_chunk) { if (actual) *actual = 0; return 0; }	/* malformed reply */
	size_t n = maxlen - 1 < remaining ? maxlen - 1 : remaining;
	uint8_t *b = p;
	b[0] = 0x05;
	for (size_t i = 0; i < n; i++) b[1 + i] = (uint8_t)(DEV_LEN - remaining + i);
	remaining -= n;
	if (actual) *actual = n + 1;
	return 0;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void run(size_t size) {
	enum { PAD = 64 };
	uint8_t *buf = malloc(size + PAD);
	memset(buf, 0xAA, size + PAD);
	size_t got = 12345;
	remaining = DEV_LEN; sent_ff00 = 0;

	int r = nspire_file_read(NULL, "/x.tns", buf, size, &got);

	CHECK(r == NSPIRE_ERR_SUCCESS, "size=%zu: ret %d", size, r);
	CHECK(got == DEV_LEN, "size=%zu: read_bytes %zu, want %d", size, got, DEV_LEN);
	CHECK(remaining == 0 && sent_ff00, "size=%zu: transfer not drained", size);
	for (size_t i = 0; i < size && i < DEV_LEN; i++)
		if (buf[i] != (uint8_t)i) { CHECK(0, "size=%zu: byte %zu wrong", size, i); break; }
	for (size_t i = size; i < size + PAD; i++)
		if (buf[i] != 0xAA) { CHECK(0, "size=%zu: wrote past the buffer at offset %zu", size, i); break; }
	free(buf);
}

int main(void) {
	size_t sizes[] = { DEV_LEN, DEV_LEN + 5, 0, 1, 10, MAXDATA, DEV_LEN - 1 };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) run(sizes[i]);
	uint8_t one[8]; size_t got = 0;
	remaining = DEV_LEN; empty_chunk = 1;
	CHECK(nspire_file_read(NULL, "/x.tns", one, sizeof(one), &got) == -NSPIRE_ERR_INVALPKT, "empty chunk not rejected");
	empty_chunk = 0;
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}

/*
 * inflate_test FILE.gz FILE: inflate FILE.gz with inflate.c and compare it
 * with FILE. Also checks that truncated and corrupted copies are rejected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n ? n : 1);
    if (fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    size_t gz_len, want_len, i;
    uint8_t *gz, *want, *got;
    long size;
    int fail = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: inflate_test FILE.gz FILE\n");
        return 2;
    }
    gz = slurp(argv[1], &gz_len);
    want = slurp(argv[2], &want_len);
    size = gzip_size(gz, gz_len);
    if (size != (long)want_len) {
        printf("FAIL %s: gzip_size %ld, want %zu\n", argv[1], size, want_len);
        return 1;
    }
    got = malloc(want_len + 1);
    if (gzip_inflate(got, want_len, gz, gz_len) != 0 ||
        memcmp(got, want, want_len) != 0) {
        printf("FAIL %s: wrong output\n", argv[1]);
        return 1;
    }
    /* too small an output buffer */
    if (want_len > 0 && gzip_inflate(got, want_len - 1, gz, gz_len) == 0) {
        printf("FAIL %s: accepted a short output buffer\n", argv[1]);
        fail = 1;
    }
    /* truncated input: drop part of the deflate data, keep the trailer */
    if (gz_len > 40) {
        uint8_t *t = malloc(gz_len);
        size_t cut = (gz_len - 8) / 2;
        memcpy(t, gz, cut);
        memcpy(t + cut, gz + gz_len - 8, 8);
        if (gzip_inflate(got, want_len, t, cut + 8) == 0) {
            printf("FAIL %s: accepted a truncated stream\n", argv[1]);
            fail = 1;
        }
        free(t);
    }
    /* a flipped bit anywhere in the data must not pass the checks */
    for (i = 10; i + 8 < gz_len; i += 1 + gz_len / 64) {
        gz[i] ^= 0x10;
        if (gzip_inflate(got, want_len, gz, gz_len) == 0 &&
            memcmp(got, want, want_len) != 0) {
            printf("FAIL %s: corrupt byte %zu gave wrong data\n", argv[1], i);
            fail = 1;
        }
        gz[i] ^= 0x10;
    }
    if (!fail)
        printf("ok %s (%zu -> %zu bytes)\n", argv[1], gz_len, want_len);
    return fail;
}

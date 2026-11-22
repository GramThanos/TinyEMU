/*
 * Minimal gzip (RFC 1952 / DEFLATE RFC 1951) decoder
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef INFLATE_H
#define INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Size the gzip stream says it inflates to (its ISIZE trailer, so modulo
   4 GB), or -1 if src is not a gzip stream. */
long gzip_size(const uint8_t *src, size_t src_len);

/* Inflate a whole gzip stream into dst. Returns 0, or -1 if the stream is
   corrupt, does not fit, or fails its length or CRC-32 check. */
int gzip_inflate(uint8_t *dst, size_t dst_len,
                 const uint8_t *src, size_t src_len);

#endif /* INFLATE_H */

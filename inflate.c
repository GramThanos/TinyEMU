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

/*
 * Written to inflate a Linux bzImage payload on the host instead of in the
 * emulated CPU. Huffman codes decode through one flat lookup table per code
 * (2^maxlen entries of symbol << 4 | length), rebuilt for every dynamic block.
 */
#include <string.h>
#include "inflate.h"

typedef struct {
    const uint8_t *in, *in_end;
    uint64_t bits;
    int nbits;
    int overrun;        /* zero bytes fed past the end of the input */
    uint8_t *out, *out_start, *out_end;
} Inflater;

#define MAXBITS 15

typedef struct {
    uint16_t tab[1 << MAXBITS];
    int maxlen;
} Huffman;

/* Keep at least 57 bits buffered: enough for a literal/length code, its
   extra bits, a distance code and its extra bits (15 + 5 + 15 + 13). */
static inline void refill(Inflater *s)
{
    while (s->nbits <= 56) {
        uint64_t b;
        if (s->in < s->in_end) {
            b = *s->in++;
        } else {
            b = 0;
            s->overrun++;
        }
        s->bits |= b << s->nbits;
        s->nbits += 8;
    }
}

static inline unsigned getbits(Inflater *s, int n)
{
    unsigned v;
    if (s->nbits < n)
        refill(s);
    v = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

/* Returns the symbol, or -1 for a code no symbol was assigned. Needs a
   prior refill(). */
static inline int decode(Inflater *s, const Huffman *h)
{
    unsigned e = h->tab[s->bits & ((1u << h->maxlen) - 1)];
    int len = e & 15;
    if (len == 0)
        return -1;
    s->bits >>= len;
    s->nbits -= len;
    return e >> 4;
}

/* Canonical Huffman code from code lengths (RFC 1951 3.2.2). Incomplete codes
   are accepted, as a single distance code must be; their unused table slots
   stay 0 and decode() rejects them. */
static int build(Huffman *h, const uint8_t *lens, int n)
{
    int count[MAXBITS + 1] = { 0 }, next[MAXBITS + 1];
    int i, len, left, code, size;

    for (i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;
    h->maxlen = 1;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        if (count[len])
            h->maxlen = len;
        left = (left << 1) - count[len];
        if (left < 0)
            return -1;   /* over-subscribed */
    }
    code = 0;
    for (len = 1; len <= MAXBITS; len++) {
        code = (code + count[len - 1]) << 1;
        next[len] = code;
    }
    size = 1 << h->maxlen;
    memset(h->tab, 0, size * sizeof(h->tab[0]));
    for (i = 0; i < n; i++) {
        int c, r, j;
        len = lens[i];
        if (len == 0)
            continue;
        /* codes are sent MSB first but read LSB first: index by the reverse */
        c = next[len]++;
        r = 0;
        for (j = 0; j < len; j++)
            r |= ((c >> j) & 1) << (len - 1 - j);
        for (j = r; j < size; j += 1 << len)
            h->tab[j] = (uint16_t)(i << 4 | len);
    }
    return 0;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int codes(Inflater *s, const Huffman *lit, const Huffman *dist)
{
    for (;;) {
        int sym, len, d;

        refill(s);
        sym = decode(s, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->out >= s->out_end)
                return -1;
            *s->out++ = sym;
            continue;
        }
        if (sym == 256)
            return 0;
        sym -= 257;
        if (sym >= 29)
            return -1;
        len = len_base[sym] + getbits(s, len_extra[sym]);
        sym = decode(s, dist);
        if (sym < 0 || sym >= 30)
            return -1;
        d = dist_base[sym] + getbits(s, dist_extra[sym]);
        if (d > s->out - s->out_start || len > s->out_end - s->out)
            return -1;
        /* byte by byte: a copy may overlap its own output */
        {
            uint8_t *o = s->out, *from = o - d;
            uint8_t *end = o + len;
            while (o < end)
                *o++ = *from++;
            s->out = o;
        }
    }
}

static int stored(Inflater *s)
{
    unsigned len, nlen;

    getbits(s, s->nbits & 7);   /* to a byte boundary */
    len = getbits(s, 16);
    nlen = getbits(s, 16);
    if ((len ^ 0xffff) != nlen || len > (size_t)(s->out_end - s->out))
        return -1;
    while (len--)
        *s->out++ = getbits(s, 8);
    return 0;
}

static int fixed(Inflater *s, Huffman *lit, Huffman *dist)
{
    uint8_t lens[288];
    int i;

    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    build(lit, lens, 288);
    for (i = 0; i < 30; i++) lens[i] = 5;
    build(dist, lens, 30);
    return codes(s, lit, dist);
}

static int dynamic(Inflater *s, Huffman *lit, Huffman *dist)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t lens[288 + 32];
    int nlen, ndist, ncode, i;

    nlen = getbits(s, 5) + 257;
    ndist = getbits(s, 5) + 1;
    ncode = getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return -1;

    memset(lens, 0, 19);
    for (i = 0; i < ncode; i++)
        lens[order[i]] = getbits(s, 3);
    if (build(lit, lens, 19) < 0)   /* lit doubles as the code-length code */
        return -1;

    for (i = 0; i < nlen + ndist; ) {
        int sym, rep, val;
        refill(s);
        sym = decode(s, lit);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lens[i++] = sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0)
                return -1;
            val = lens[i - 1];
            rep = 3 + getbits(s, 2);
        } else if (sym == 17) {
            val = 0;
            rep = 3 + getbits(s, 3);
        } else {
            val = 0;
            rep = 11 + getbits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lens[i++] = val;
    }
    if (lens[256] == 0)   /* no end-of-block code */
        return -1;
    if (build(lit, lens, nlen) < 0 || build(dist, lens + nlen, ndist) < 0)
        return -1;
    return codes(s, lit, dist);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    uint32_t c;
    size_t i;

    if (table[1] == 0) {
        for (i = 0; i < 256; i++) {
            int k;
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    c = 0xffffffff;
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffff;
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

long gzip_size(const uint8_t *src, size_t src_len)
{
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8)
        return -1;
    return get_le32(src + src_len - 4);
}

int gzip_inflate(uint8_t *dst, size_t dst_len,
                 const uint8_t *src, size_t src_len)
{
    /* static: two 64 KB tables are too much for a small (WASM) stack */
    static Huffman lit, dist;
    Inflater s;
    size_t pos;
    int flags, last, ret;

    if (gzip_size(src, src_len) < 0)
        return -1;
    flags = src[3];
    pos = 10;
    if (flags & 4) {   /* FEXTRA */
        if (pos + 2 > src_len)
            return -1;
        pos += 2 + (src[pos] | (src[pos + 1] << 8));
    }
    if (flags & 8) {   /* FNAME */
        while (pos < src_len && src[pos])
            pos++;
        pos++;
    }
    if (flags & 16) {  /* FCOMMENT */
        while (pos < src_len && src[pos])
            pos++;
        pos++;
    }
    if (flags & 2)     /* FHCRC */
        pos += 2;
    if (pos + 8 > src_len)
        return -1;

    memset(&s, 0, sizeof(s));
    s.in = src + pos;
    s.in_end = src + src_len - 8;   /* the CRC-32 + ISIZE trailer */
    s.out = s.out_start = dst;
    s.out_end = dst + dst_len;

    do {
        int type;
        last = getbits(&s, 1);
        type = getbits(&s, 2);
        if (type == 0)
            ret = stored(&s);
        else if (type == 1)
            ret = fixed(&s, &lit, &dist);
        else if (type == 2)
            ret = dynamic(&s, &lit, &dist);
        else
            ret = -1;
        if (ret < 0)
            return -1;
        /* zero padding past the end can still look like valid codes; a
           stream that needed it is truncated */
        if (s.overrun * 8 > s.nbits)
            return -1;
    } while (!last);

    if ((size_t)(s.out - dst) != get_le32(src + src_len - 4) ||
        crc32(dst, s.out - dst) != get_le32(src + src_len - 8))
        return -1;
    return 0;
}

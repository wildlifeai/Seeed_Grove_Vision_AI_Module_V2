/*
 * sw_jpeg.c  -  baseline (sequential DCT) JPEG encoder for planar YUV420.
 * See sw_jpeg.h. Uses standard JPEG Annex K tables. FPU float DCT (the CM55
 * has hardware float, -mfloat-abi=hard).
 */

#include <stdint.h>
#include <string.h>
#include <math.h>

#include "sw_jpeg.h"

/*************************************** Standard tables *********************/

// Zig-zag scan order (natural index for each of the 64 zig-zag positions)
static const uint8_t ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// Annex K.1 / K.2 base quantisation tables (natural order)
static const uint8_t STD_LUMA_Q[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};
static const uint8_t STD_CHROMA_Q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// Annex K.3 standard Huffman tables (bits[0..15] = # codes of length 1..16)
static const uint8_t DC_LUMA_BITS[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t DC_LUMA_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t DC_CHROMA_BITS[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t DC_CHROMA_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t AC_LUMA_BITS[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t AC_LUMA_VAL[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t AC_CHROMA_BITS[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t AC_CHROMA_VAL[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

/*************************************** Derived state **********************/

// Canonical Huffman code/length per 8-bit symbol, built from the BITS/VAL tables
typedef struct { uint16_t code[256]; uint8_t size[256]; } huff_t;

static huff_t hDcLuma, hAcLuma, hDcChroma, hAcChroma;
static float dctMat[8][8];     // separable DCT basis
static uint16_t qLuma[64], qChroma[64];   // scaled quant tables (natural order)
static int tablesReadyQuality = -1;   // quality the tables were built for, -1 = none

/*************************************** Bit writer *************************/

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t bitBuf;   // accumulates bits MSB-first
    int      bitCnt;   // bits currently in bitBuf
    int      overflow;
} bitwriter_t;

static void bw_byte(bitwriter_t *w, uint8_t b) {
    if (w->len < w->cap) {
        w->buf[w->len++] = b;
    } else {
        w->overflow = 1;
    }
}

// Emit a run of bits (value's low 'nbits' bits), MSB first, with 0xFF stuffing
static void bw_bits(bitwriter_t *w, uint32_t value, int nbits) {
    if (nbits == 0) return;
    w->bitBuf |= (value & ((1u << nbits) - 1)) << (32 - w->bitCnt - nbits);
    w->bitCnt += nbits;
    while (w->bitCnt >= 8) {
        uint8_t b = (uint8_t)(w->bitBuf >> 24);
        bw_byte(w, b);
        if (b == 0xFF) bw_byte(w, 0x00);   // byte stuffing
        w->bitBuf <<= 8;
        w->bitCnt -= 8;
    }
}

static void bw_flush(bitwriter_t *w) {
    if (w->bitCnt > 0) {
        bw_bits(w, 0x7F, 8 - w->bitCnt);   // pad with 1s
    }
}

static void bw_marker(bitwriter_t *w, uint8_t m) {
    bw_byte(w, 0xFF);
    bw_byte(w, m);
}

/*************************************** Table setup ***********************/

static void build_huff(huff_t *h, const uint8_t *bits, const uint8_t *vals) {
    memset(h, 0, sizeof(*h));
    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            uint8_t sym = vals[k++];
            h->code[sym] = code;
            h->size[sym] = (uint8_t)len;
            code++;
        }
        code <<= 1;
    }
}

static void scale_quant(uint16_t *out, const uint8_t *base, int quality) {
    int scale;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; i++) {
        int q = (base[i] * scale + 50) / 100;
        if (q < 1) q = 1;
        if (q > 255) q = 255;   // 8-bit precision quant tables
        out[i] = (uint16_t)q;
    }
}

static void init_tables(int quality) {
    build_huff(&hDcLuma,   DC_LUMA_BITS,   DC_LUMA_VAL);
    build_huff(&hAcLuma,   AC_LUMA_BITS,   AC_LUMA_VAL);
    build_huff(&hDcChroma, DC_CHROMA_BITS, DC_CHROMA_VAL);
    build_huff(&hAcChroma, AC_CHROMA_BITS, AC_CHROMA_VAL);

    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            float c = (u == 0) ? 0.353553390593273762f /* 1/sqrt(8) */
                               : 0.5f;
            dctMat[u][x] = c * cosf((2.0f * x + 1.0f) * u * 3.14159265358979324f / 16.0f);
        }
    }

    scale_quant(qLuma,   STD_LUMA_Q,   quality);
    scale_quant(qChroma, STD_CHROMA_Q, quality);
    tablesReadyQuality = quality;
}

/*************************************** Block coding **********************/

// 8x8 forward DCT: out = M * in * M^T  (in/out natural order, row-major)
static void fdct(const float *in, float *out) {
    float tmp[64];
    // tmp = M * in
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            float s = 0.0f;
            for (int k = 0; k < 8; k++) s += dctMat[i][k] * in[k * 8 + j];
            tmp[i * 8 + j] = s;
        }
    }
    // out = tmp * M^T
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            float s = 0.0f;
            for (int k = 0; k < 8; k++) s += tmp[i * 8 + k] * dctMat[j][k];
            out[i * 8 + j] = s;
        }
    }
}

// Number of magnitude bits and the JPEG-coded value for a signed coefficient
static void magnitude(int v, int *sizeBits, uint32_t *coded) {
    int a = (v < 0) ? -v : v;
    int s = 0;
    while (a) { s++; a >>= 1; }
    *sizeBits = s;
    // negatives use (v - 1) in 'size' bits (one's-complement style)
    *coded = (uint32_t)((v < 0) ? (v - 1) : v) & ((s ? (1u << s) : 1u) - 1);
}

/*
 * Encode one 8x8 block (already level-shifted floats). Quantises with 'q',
 * Huffman-codes DC (relative to *prevDc) with hDc and AC with hAc.
 */
static void encode_block(bitwriter_t *w, const float *pixels, const uint16_t *q,
                         const huff_t *hDc, const huff_t *hAc, int *prevDc) {
    float dct[64];
    int   quant[64];   // in zig-zag order

    fdct(pixels, dct);

    for (int i = 0; i < 64; i++) {
        int n = ZIGZAG[i];
        float f = dct[n] / (float)q[n];
        quant[i] = (int)lrintf(f);
    }

    // ---- DC ----
    int diff = quant[0] - *prevDc;
    *prevDc = quant[0];
    int sBits; uint32_t coded;
    magnitude(diff, &sBits, &coded);
    bw_bits(w, hDc->code[sBits], hDc->size[sBits]);
    bw_bits(w, coded, sBits);

    // ---- AC ----
    int run = 0;
    for (int i = 1; i < 64; i++) {
        int c = quant[i];
        if (c == 0) {
            run++;
            continue;
        }
        while (run > 15) {              // ZRL (16 zero run)
            bw_bits(w, hAc->code[0xF0], hAc->size[0xF0]);
            run -= 16;
        }
        magnitude(c, &sBits, &coded);
        uint8_t rs = (uint8_t)((run << 4) | sBits);
        bw_bits(w, hAc->code[rs], hAc->size[rs]);
        bw_bits(w, coded, sBits);
        run = 0;
    }
    if (run > 0) {                       // trailing zeros -> EOB
        bw_bits(w, hAc->code[0x00], hAc->size[0x00]);
    }
}

/*************************************** Headers **************************/

static void write_dqt(bitwriter_t *w, const uint16_t *q, int id) {
    bw_marker(w, 0xDB);
    bw_byte(w, 0x00); bw_byte(w, 0x43);   // length = 67
    bw_byte(w, (uint8_t)id);              // precision 0, table id
    for (int i = 0; i < 64; i++) bw_byte(w, (uint8_t)q[ZIGZAG[i]]);  // zig-zag order
}

static void write_dht(bitwriter_t *w, int cls, int id,
                      const uint8_t *bits, const uint8_t *vals) {
    int nv = 0;
    for (int i = 0; i < 16; i++) nv += bits[i];
    int len = 2 + 1 + 16 + nv;
    bw_marker(w, 0xC4);
    bw_byte(w, (uint8_t)(len >> 8)); bw_byte(w, (uint8_t)(len & 0xFF));
    bw_byte(w, (uint8_t)((cls << 4) | id));
    for (int i = 0; i < 16; i++) bw_byte(w, bits[i]);
    for (int i = 0; i < nv; i++) bw_byte(w, vals[i]);
}

/*************************************** Public API **********************/

static void get_block(const uint8_t *plane, int stride, int W, int H,
                      int px, int py, float *blk) {
    for (int y = 0; y < 8; y++) {
        int sy = py + y; if (sy >= H) sy = H - 1;
        const uint8_t *row = plane + sy * stride;
        for (int x = 0; x < 8; x++) {
            int sx = px + x; if (sx >= W) sx = W - 1;
            blk[y * 8 + x] = (float)row[sx] - 128.0f;
        }
    }
}

uint32_t sw_jpeg_encode_yuv420(const uint8_t *yuv, uint32_t w, uint32_t h,
                               uint8_t *out, uint32_t outCap, uint8_t quality) {
    if (!yuv || !out || w == 0 || h == 0 || (w & 1) || (h & 1)) {
        return 0;
    }

    if (tablesReadyQuality != (int)quality) {
        init_tables(quality);
    }

    const uint8_t *yP  = yuv;
    const uint8_t *cbP = yuv + (uint32_t)w * h;
    const uint8_t *crP = cbP + ((uint32_t)w * h) / 4;
    int cw = (int)w / 2, ch = (int)h / 2;

    bitwriter_t bw = {0};
    bw.buf = out; bw.cap = outCap;

    // ---- headers ----
    bw_marker(&bw, 0xD8);                          // SOI

    bw_marker(&bw, 0xE0);                           // APP0 / JFIF
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x10);
    bw_byte(&bw, 'J'); bw_byte(&bw, 'F'); bw_byte(&bw, 'I'); bw_byte(&bw, 'F'); bw_byte(&bw, 0x00);
    bw_byte(&bw, 0x01); bw_byte(&bw, 0x01);         // version 1.1
    bw_byte(&bw, 0x00);                             // aspect units
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x01);
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x01);
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x00);         // no thumbnail

    write_dqt(&bw, qLuma,   0);
    write_dqt(&bw, qChroma, 1);

    // SOF0 (baseline), 3 components, 4:2:0
    bw_marker(&bw, 0xC0);
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x11);         // length 17
    bw_byte(&bw, 0x08);                             // precision
    bw_byte(&bw, (uint8_t)(h >> 8)); bw_byte(&bw, (uint8_t)(h & 0xFF));
    bw_byte(&bw, (uint8_t)(w >> 8)); bw_byte(&bw, (uint8_t)(w & 0xFF));
    bw_byte(&bw, 0x03);                             // 3 components
    bw_byte(&bw, 0x01); bw_byte(&bw, 0x22); bw_byte(&bw, 0x00);  // Y  2x2 quant0
    bw_byte(&bw, 0x02); bw_byte(&bw, 0x11); bw_byte(&bw, 0x01);  // Cb 1x1 quant1
    bw_byte(&bw, 0x03); bw_byte(&bw, 0x11); bw_byte(&bw, 0x01);  // Cr 1x1 quant1

    write_dht(&bw, 0, 0, DC_LUMA_BITS,   DC_LUMA_VAL);
    write_dht(&bw, 1, 0, AC_LUMA_BITS,   AC_LUMA_VAL);
    write_dht(&bw, 0, 1, DC_CHROMA_BITS, DC_CHROMA_VAL);
    write_dht(&bw, 1, 1, AC_CHROMA_BITS, AC_CHROMA_VAL);

    // SOS
    bw_marker(&bw, 0xDA);
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x0C);         // length 12
    bw_byte(&bw, 0x03);                             // 3 components
    bw_byte(&bw, 0x01); bw_byte(&bw, 0x00);         // Y  : DC0 AC0
    bw_byte(&bw, 0x02); bw_byte(&bw, 0x11);         // Cb : DC1 AC1
    bw_byte(&bw, 0x03); bw_byte(&bw, 0x11);         // Cr : DC1 AC1
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x3F); bw_byte(&bw, 0x00);  // Ss Se Ah/Al

    // ---- scan: 16x16 MCUs, each 4 Y + 1 Cb + 1 Cr ----
    int dcY = 0, dcCb = 0, dcCr = 0;
    int mcuX = ((int)w + 15) / 16, mcuY = ((int)h + 15) / 16;
    float blk[64];

    for (int my = 0; my < mcuY && !bw.overflow; my++) {
        for (int mx = 0; mx < mcuX; mx++) {
            int bx = mx * 16, by = my * 16;
            // 4 luma blocks: TL, TR, BL, BR
            get_block(yP, (int)w, (int)w, (int)h, bx,     by,     blk); encode_block(&bw, blk, qLuma, &hDcLuma, &hAcLuma, &dcY);
            get_block(yP, (int)w, (int)w, (int)h, bx + 8, by,     blk); encode_block(&bw, blk, qLuma, &hDcLuma, &hAcLuma, &dcY);
            get_block(yP, (int)w, (int)w, (int)h, bx,     by + 8, blk); encode_block(&bw, blk, qLuma, &hDcLuma, &hAcLuma, &dcY);
            get_block(yP, (int)w, (int)w, (int)h, bx + 8, by + 8, blk); encode_block(&bw, blk, qLuma, &hDcLuma, &hAcLuma, &dcY);
            // chroma (downsampled): one 8x8 each at (mx*8, my*8)
            get_block(cbP, cw, cw, ch, mx * 8, my * 8, blk); encode_block(&bw, blk, qChroma, &hDcChroma, &hAcChroma, &dcCb);
            get_block(crP, cw, cw, ch, mx * 8, my * 8, blk); encode_block(&bw, blk, qChroma, &hDcChroma, &hAcChroma, &dcCr);
        }
    }

    bw_flush(&bw);
    bw_marker(&bw, 0xD9);                            // EOI

    if (bw.overflow) return 0;
    return bw.len;
}

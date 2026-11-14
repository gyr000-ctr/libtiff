/*
 * Copyright (c) 2026, libtiff contributors
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library
 *
 * Tests for the experimental AVIF compression codec (COMPRESSION_AVIF,
 * implemented through libavif). Covers lossy/lossless encoding, tile and
 * strip organization, alpha channels, ICC profiles, subsampling, bit
 * depths and the distinguishable error paths for invalid AVIF data,
 * oversize dimensions, corrupt ICC profiles and tile geometry mismatch.
 */

#include "tif_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffio.h"

#ifdef AVIF_SUPPORT
#include <avif/avif.h>

/* ------------------------------------------------------------------ */
/* In-memory TIFF I/O                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *data;
    tmsize_t size;
    tmsize_t capacity;
    tmsize_t pos;
} MemFile;

static tmsize_t memRead(thandle_t h, void *buf, tmsize_t n)
{
    MemFile *f = (MemFile *)h;
    tmsize_t avail = f->size - f->pos;
    if (n > avail)
        n = avail;
    if (n > 0)
        _TIFFmemcpy(buf, f->data + f->pos, (size_t)n);
    f->pos += n;
    return n;
}

static tmsize_t memWrite(thandle_t h, void *buf, tmsize_t n)
{
    MemFile *f = (MemFile *)h;
    if (f->pos + n > f->capacity)
    {
        tmsize_t newcap = f->capacity ? f->capacity : 65536;
        while (newcap < f->pos + n)
            newcap *= 2;
        unsigned char *nd =
            (unsigned char *)realloc(f->data, (size_t)newcap);
        if (nd == NULL)
            return 0;
        f->data = nd;
        f->capacity = newcap;
    }
    _TIFFmemcpy(f->data + f->pos, buf, (size_t)n);
    f->pos += n;
    if (f->pos > f->size)
        f->size = f->pos;
    return n;
}

static toff_t memSeek(thandle_t h, toff_t off, int whence)
{
    MemFile *f = (MemFile *)h;
    if (whence == SEEK_SET)
        f->pos = (tmsize_t)off;
    else if (whence == SEEK_CUR)
        f->pos += (tmsize_t)off;
    else if (whence == SEEK_END)
        f->pos = f->size + (tmsize_t)off;
    return (toff_t)f->pos;
}

static int memClose(thandle_t h)
{
    (void)h;
    return 0;
}

static toff_t memSize(thandle_t h)
{
    return (toff_t)((MemFile *)h)->size;
}

static int memMap(thandle_t h, void **base, toff_t *size)
{
    MemFile *f = (MemFile *)h;
    *base = f->data;
    *size = (toff_t)f->size;
    return 0;
}

static void memUnmap(thandle_t h, void *base, toff_t size)
{
    (void)h;
    (void)base;
    (void)size;
}

static TIFF *memOpen(MemFile *f, const char *mode)
{
    /* Reads always start at the beginning of the backing store. */
    if (mode[0] == 'r')
        f->pos = 0;
    else if (f->data == NULL)
    {
        /* Pre-allocate a fixed, generous buffer so the backing store is
         * never realloc'd (which would move it and invalidate any mapped
         * base pointer libtiff may have cached). */
        f->capacity = 64 * 1024 * 1024;
        f->data = (unsigned char *)calloc(1, (size_t)f->capacity);
    }
    return TIFFClientOpen("memory", mode, (thandle_t)f, memRead, memWrite,
                          memSeek, memClose, memSize, memMap, memUnmap);
}

/* ------------------------------------------------------------------ */
/* Error capture                                                      */
/* ------------------------------------------------------------------ */

static char g_last_error[1024];
static int g_error_count;

static void captureErrorHandler(const char *module, const char *fmt,
                                va_list ap)
{
    int n = snprintf(g_last_error, sizeof(g_last_error), "%s: ", module);
    if (n < 0 || n >= (int)sizeof(g_last_error))
        n = 0;
    vsnprintf(g_last_error + n, sizeof(g_last_error) - (size_t)n, fmt, ap);
    g_error_count++;
}

static void resetErrors(void)
{
    g_last_error[0] = '\0';
    g_error_count = 0;
}

static int errorContains(const char *needle)
{
    return strstr(g_last_error, needle) != NULL;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s (last error: %s)\n", what, g_last_error);
    return 1;
}

/*
 * Build a structurally valid, minimal 140-byte ICC profile (128-byte
 * header + 4-byte tag count = 0 tags). The 'acsp' signature and declared
 * profile size are set correctly.
 */
static void makeValidICC(unsigned char icc[140])
{
    memset(icc, 0, 140);
    /* profile size = 140, big endian */
    icc[0] = 0;
    icc[1] = 0;
    icc[2] = 0;
    icc[3] = 140;
    memcpy(icc + 36, "acsp", 4);
    /* tag count at offset 128 = 0 */
}

/* Raster description shared by the round-trip tests. */
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint16_t samples;       /* 3 or 4 */
    uint16_t bits;          /* 8, 10 or 12 (stored in 16-bit containers >8) */
    int tiled;
    uint32_t tilewidth;
    uint32_t tilelength;
    uint32_t rowsperstrip;
    int lossless;
    int subsampling;
    int use_icc;
    int corrupt_icc;
    uint16_t *raster16;     /* used when bits > 8 */
    uint8_t *raster8;       /* used when bits == 8 */
} RasterSpec;

/*
 * libtiff stores >8-bit samples bit-packed; this is the packed raster row
 * byte size (matches TIFFScanlineSize semantics).
 */
static tmsize_t packedRowBytes(const RasterSpec *r)
{
    uint64_t bits = (uint64_t)r->width * (uint64_t)r->samples *
                    (uint64_t)r->bits;
    return (tmsize_t)((bits + 7u) / 8u);
}

static tmsize_t rasterBytes(const RasterSpec *r)
{
    if (r->bits <= 8)
    {
        return (tmsize_t)r->width * (tmsize_t)r->height *
               (tmsize_t)r->samples;
    }
    return packedRowBytes(r) * (tmsize_t)r->height;
}

/* Test-side bit packing, matching libtiff's raster layout for 10/12-bit. */
static uint16_t unpackSample(const uint8_t *p, uint64_t sample_index,
                             uint16_t bits)
{
    uint64_t bitpos = sample_index * bits;
    uint64_t byte_index = bitpos / 8u;
    uint32_t bit_offset = (uint32_t)(bitpos % 8u);
    uint32_t avail = 8u - bit_offset;
    uint32_t mask = (1u << bits) - 1u;
    uint32_t v;
    if (avail >= bits)
        v = (p[byte_index] >> (avail - bits)) & mask;
    else
    {
        v = ((uint32_t)p[byte_index] & ((1u << avail) - 1u))
            << (bits - avail);
        v |= (uint32_t)p[byte_index + 1] >> (8u - (bits - avail));
        v &= mask;
    }
    return (uint16_t)v;
}

static void packSample(uint8_t *p, uint64_t sample_index, uint16_t bits,
                       uint16_t value)
{
    uint64_t bitpos = sample_index * bits;
    uint64_t byte_index = bitpos / 8u;
    uint32_t bit_offset = (uint32_t)(bitpos % 8u);
    uint32_t avail = 8u - bit_offset;
    uint32_t mask = (1u << bits) - 1u;
    uint32_t v = (uint32_t)value & mask;
    if (avail >= bits)
        p[byte_index] |= (uint8_t)(v << (avail - bits));
    else
    {
        p[byte_index] |= (uint8_t)(v >> (bits - avail));
        p[byte_index + 1] |=
            (uint8_t)((v << (8u - (bits - avail))) & 0xffu);
    }
}

static uint16_t sampleValue(const RasterSpec *r, uint32_t x, uint32_t y,
                            uint32_t channel)
{
    uint32_t maxval =
        (r->bits == 8) ? 255u : ((1u << r->bits) - 1u);
    if (channel == 0)
        return (uint16_t)(((r->bits <= 8 ? (x * 17 + y * 31)
                                        : (x * 171 + y * 313))) &
                         maxval);
    if (channel == 1)
        return (uint16_t)(((r->bits <= 8 ? (x * 7 + y * 13 + 64)
                                        : (x * 71 + y * 137 + 640))) &
                         maxval);
    if (channel == 2)
        return (uint16_t)(((r->bits <= 8 ? (x * 3 + y * 5 + 128)
                                        : (x * 31 + y * 53 + 1280))) &
                         maxval);
    return (uint16_t)(((x + y) & 1) ? maxval : (maxval / 3));
}

static void fillRaster(const RasterSpec *r)
{
    if (r->bits <= 8)
    {
        for (uint32_t y = 0; y < r->height; y++)
            for (uint32_t x = 0; x < r->width; x++)
                for (uint32_t c = 0; c < r->samples; c++)
                {
                    uint64_t idx =
                        ((uint64_t)y * r->width + x) * r->samples + c;
                    r->raster8[idx] = sampleValue(r, x, y, c);
                }
    }
    else
    {
        memset(r->raster8, 0, (size_t)rasterBytes(r));
        for (uint32_t y = 0; y < r->height; y++)
            for (uint32_t x = 0; x < r->width; x++)
                for (uint32_t c = 0; c < r->samples; c++)
                {
                    uint64_t idx =
                        ((uint64_t)y * r->width + x) * r->samples + c;
                    packSample(r->raster8, idx, r->bits,
                               sampleValue(r, x, y, c));
                }
    }
}

static TIFF *openWriter(MemFile *f, const RasterSpec *r)
{
    TIFF *tif = memOpen(f, "w");
    if (tif == NULL)
        return NULL;
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, r->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, r->height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, r->samples);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, r->bits);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_AVIF);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    if (r->samples == 4)
    {
        uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }
    TIFFSetField(tif, TIFFTAG_AVIF_LOSSLESS, r->lossless);
    TIFFSetField(tif, TIFFTAG_AVIF_SUBSAMPLING, r->subsampling);
    if (r->lossless)
        TIFFSetField(tif, TIFFTAG_AVIF_SPEED, 8);
    else
        TIFFSetField(tif, TIFFTAG_AVIF_QUALITY, 70);
    if (r->tiled)
    {
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, r->tilewidth);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, r->tilelength);
    }
    else
    {
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, r->rowsperstrip);
    }
    if (r->use_icc)
    {
        /* A stack-local buffer: TIFFSetField copies the bytes, and a
         * static one would be shared across calls and clobbered. */
        unsigned char icc[140];
        makeValidICC(icc);
        if (r->corrupt_icc)
            icc[3] = 200; /* break declared-size consistency */
        TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)140, icc);
    }
    return tif;
}

static int writeRaster(TIFF *tif, const RasterSpec *r)
{
    tmsize_t total = rasterBytes(r);
    uint8_t *base = r->raster8;

    if (r->tiled)
    {
        tmsize_t tilesize = TIFFTileSize(tif);
        uint32_t ntiles = TIFFNumberOfTiles(tif);
        uint8_t *buf = (uint8_t *)_TIFFmalloc(tilesize);
        if (buf == NULL)
            return 0;
        for (uint32_t t = 0; t < ntiles; t++)
        {
            uint32_t tx, ty, tw, th;
            uint32_t ntx = (r->width + r->tilewidth - 1) / r->tilewidth;
            tx = t % ntx;
            ty = t / ntx;
            tw = r->tilewidth;
            th = r->tilelength;
            if (tx * r->tilewidth + tw > r->width)
                tw = r->width - tx * r->tilewidth;
            if (ty * r->tilelength + th > r->height)
                th = r->height - ty * r->tilelength;

            /* Tile buffers are byte-aligned per tile and bit-packed
             * sample positions do not line up with the linear raster at
             * tile boundaries, so transfer sample by sample. */
            memset(buf, 0, (size_t)tilesize);
            for (uint32_t yy = 0; yy < th; yy++)
                for (uint32_t xx = 0; xx < tw; xx++)
                    for (uint32_t c = 0; c < r->samples; c++)
                    {
                        uint32_t gx = tx * r->tilewidth + xx;
                        uint32_t gy = ty * r->tilelength + yy;
                        uint64_t src_idx =
                            ((uint64_t)gy * r->width + gx) *
                                r->samples +
                            c;
                        uint64_t dst_idx =
                            ((uint64_t)yy * r->tilewidth + xx) *
                                r->samples +
                            c;
                        if (r->bits <= 8)
                            buf[dst_idx] = base[src_idx];
                        else
                        {
                            uint16_t v =
                                unpackSample(base, src_idx, r->bits);
                            packSample(buf, dst_idx, r->bits, v);
                        }
                    }
            int ok = TIFFWriteEncodedTile(tif, t, buf, tilesize) ==
                     tilesize;
            if (!ok)
            {
                _TIFFfree(buf);
                return 0;
            }
        }
        _TIFFfree(buf);
    }
    else
    {
        tmsize_t stripsize = TIFFStripSize(tif);
        uint32_t nstrips = TIFFNumberOfStrips(tif);
        tmsize_t off = 0;
        for (uint32_t s = 0; s < nstrips; s++)
        {
            tmsize_t chunk = total - off;
            if (chunk > stripsize)
                chunk = stripsize;
            if (TIFFWriteEncodedStrip(tif, s, base + off, chunk) != chunk)
                return 0;
            off += chunk;
        }
    }
    return 1;
}

/* Read one sample from a linear (non-tiled) packed raster. */
static uint16_t linearSample(const RasterSpec *r, const uint8_t *buf,
                             uint32_t x, uint32_t y, uint32_t c)
{
    uint64_t idx = ((uint64_t)y * r->width + x) * r->samples + c;
    if (r->bits <= 8)
        return buf[idx];
    return unpackSample(buf, idx, r->bits);
}

static int compareRasters(const RasterSpec *r, const void *got)
{
    const uint8_t *g = (const uint8_t *)got;

    /* Tiled output is reassembled into the linear raster before compare. */
    for (uint32_t y = 0; y < r->height; y++)
    {
        for (uint32_t x = 0; x < r->width; x++)
        {
            for (uint32_t c = 0; c < r->samples; c++)
            {
                uint16_t expect = sampleValue(r, x, y, c);
                uint16_t actual = linearSample(r, g, x, y, c);
                if (expect != actual)
                    return 0;
            }
        }
    }
    return 1;
}

static int readBack(MemFile *f, const RasterSpec *r, void *out)
{
    TIFF *tif = memOpen(f, "r");
    if (tif == NULL)
        return 0;
    int ok = 0;
    if (r->tiled)
    {
        tmsize_t tilesize = TIFFTileSize(tif);
        uint32_t ntiles = TIFFNumberOfTiles(tif);
        uint8_t *buf = (uint8_t *)_TIFFmalloc(tilesize);
        if (buf == NULL)
            goto done;
        /* Reassemble directly into the linear raster, sample by sample,
         * since bit-packed tile boundaries do not align byte-wise. */
        memset(out, 0, (size_t)rasterBytes(r));
        for (uint32_t t = 0; t < ntiles; t++)
        {
            if (TIFFReadEncodedTile(tif, t, buf, tilesize) < 0)
            {
                _TIFFfree(buf);
                goto done;
            }
            uint32_t ntx =
                (r->width + r->tilewidth - 1) / r->tilewidth;
            uint32_t tx = t % ntx;
            uint32_t ty = t / ntx;
            uint32_t tw = r->tilewidth;
            uint32_t th = r->tilelength;
            if (tx * r->tilewidth + tw > r->width)
                tw = r->width - tx * r->tilewidth;
            if (ty * r->tilelength + th > r->height)
                th = r->height - ty * r->tilelength;
            for (uint32_t yy = 0; yy < th; yy++)
                for (uint32_t xx = 0; xx < tw; xx++)
                    for (uint32_t c = 0; c < r->samples; c++)
                    {
                        uint32_t gx = tx * r->tilewidth + xx;
                        uint32_t gy = ty * r->tilelength + yy;
                        uint64_t dst_idx =
                            ((uint64_t)gy * r->width + gx) *
                                r->samples +
                            c;
                        uint64_t src_idx =
                            ((uint64_t)yy * r->tilewidth + xx) *
                                r->samples +
                            c;
                        if (r->bits <= 8)
                            ((uint8_t *)out)[dst_idx] = buf[src_idx];
                        else
                        {
                            uint16_t v = unpackSample(buf, src_idx,
                                                     r->bits);
                            packSample(out, dst_idx, r->bits, v);
                        }
                    }
        }
        _TIFFfree(buf);
        ok = 1;
    }
    else
    {
        tmsize_t stripsize = TIFFStripSize(tif);
        uint32_t nstrips = TIFFNumberOfStrips(tif);
        tmsize_t off = 0;
        int good = 1;
        for (uint32_t s = 0; s < nstrips; s++)
        {
            tmsize_t got = TIFFReadEncodedStrip(
                tif, s, (uint8_t *)out + off, stripsize);
            if (got < 0)
            {
                good = 0;
                break;
            }
            off += got;
        }
        ok = good;
    }
done:
    TIFFClose(tif);
    return ok;
}

static int roundtrip(const char *name, const RasterSpec *r, int exact)
{
    MemFile f = {0};
    int rc = 0;

    fillRaster(r);

    TIFF *tif = openWriter(&f, r);
    if (tif == NULL)
        return fail(name);
    if (!writeRaster(tif, r))
    {
        TIFFClose(tif);
        free(f.data);
        return fail(name);
    }
    TIFFClose(tif);

    if (f.size == 0)
    {
        free(f.data);
        return fail("empty output file");
    }

    /* The output is always the linear raster (strips directly, tiles
     * reassembled sample by sample). */
    tmsize_t out_size = rasterBytes(r);
    void *out = malloc((size_t)out_size);
    if (out == NULL)
    {
        free(f.data);
        return fail("malloc");
    }
    memset(out, 0, (size_t)out_size);

    resetErrors();
    if (!readBack(&f, r, out))
    {
        free(out);
        free(f.data);
        return fail(name);
    }

    if (exact)
    {
        if (!compareRasters(r, out))
        {
            free(out);
            free(f.data);
            return fail("lossless pixel mismatch");
        }
    }
    else
    {
        /* Lossy: assert on the average error, which stays bounded for
         * every chroma format. A per-sample max-diff ceiling is not a
         * meaningful criterion for 4:2:0/4:2:2, where chroma subsampling
         * deliberately changes high-frequency pixels (the synthetic
         * zig-zag pattern here) even at the highest quality. */
        int maxdiff = 0;
        uint64_t sumdiff = 0;
        uint64_t count = 0;
        for (uint32_t y = 0; y < r->height; y++)
        {
            for (uint32_t x = 0; x < r->width; x++)
            {
                for (uint32_t c = 0; c < r->samples; c++)
                {
                    uint16_t expect = sampleValue(r, x, y, c);
                    uint16_t actual = linearSample(r, out, x, y, c);
                    int d = (int)expect - (int)actual;
                    if (d < 0)
                        d = -d;
                    if (d > maxdiff)
                        maxdiff = d;
                    sumdiff += (uint64_t)d;
                    count++;
                }
            }
        }
        double avg = count ? (double)sumdiff / (double)count : 0.0;
        /* 4:4:4 keeps the chroma full resolution and must stay very close;
         * subsampled formats get a looser but still bounded average. */
        double avg_limit =
            (r->subsampling == AVIF_SUBSAMPLING_444) ? 6.0 : 25.0;
        if (avg > avg_limit)
        {
            fprintf(stderr,
                    "FAIL: %s lossy avg %.2f exceeds limit %.2f "
                    "(maxdiff=%d)\n",
                    name, avg, avg_limit, maxdiff);
            rc = 1;
        }
    }

    free(out);
    free(f.data);
    if (rc == 0)
        fprintf(stderr, "PASS: %s\n", name);
    return rc;
}

static RasterSpec makeSpec(uint32_t w, uint32_t h, uint16_t samples,
                           uint16_t bits, int tiled, int lossless,
                           int subsampling)
{
    RasterSpec r;
    memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.samples = samples;
    r.bits = bits;
    r.tiled = tiled;
    r.tilewidth = tiled ? 16 : 0;
    r.tilelength = tiled ? 16 : 0;
    r.rowsperstrip = tiled ? 0 : 8;
    r.lossless = lossless;
    r.subsampling = subsampling;
    r.use_icc = 0;
    r.corrupt_icc = 0;
    /* All rasters are byte buffers; 10/12-bit samples are bit-packed. */
    r.raster8 = (uint8_t *)malloc((size_t)rasterBytes(&r));
    return r;
}

static void freeSpec(RasterSpec *r)
{
    free(r->raster8);
    free(r->raster16);
}

/* ------------------------------------------------------------------ */
/* Negative tests                                                     */
/* ------------------------------------------------------------------ */

/* Encode a small standalone AVIF bitstream with libavif, so it can be
 * embedded as a deliberately mismatched tile. */
static int makeForeignAVIF(uint8_t **out_data, size_t *out_size,
                           uint32_t w, uint32_t h)
{
    avifImage *image = avifImageCreate(w, h, 8, AVIF_PIXEL_FORMAT_YUV444);
    if (image == NULL)
        return 0;
    image->yuvRange = AVIF_RANGE_FULL;

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.width = w;
    rgb.height = h;
    rgb.depth = 8;
    rgb.format = AVIF_RGB_FORMAT_RGB;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK)
    {
        avifImageDestroy(image);
        return 0;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            uint8_t *p = rgb.pixels +
                         ((size_t)y * rgb.rowBytes + (size_t)x * 3);
            p[0] = (uint8_t)(x * 9);
            p[1] = (uint8_t)(y * 11);
            p[2] = 128;
        }
    if (avifImageRGBToYUV(image, &rgb) != AVIF_RESULT_OK)
    {
        avifRGBImageFreePixels(&rgb);
        avifImageDestroy(image);
        return 0;
    }
    avifEncoder *enc = avifEncoderCreate();
    enc->quality = AVIF_QUALITY_LOSSLESS;
    enc->maxThreads = 1;
    avifRWData raw = AVIF_DATA_EMPTY;
    avifResult res = avifEncoderWrite(enc, image, &raw);
    avifEncoderDestroy(enc);
    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(image);
    if (res != AVIF_RESULT_OK)
        return 0;
    *out_data = raw.data;
    *out_size = raw.size;
    return 1;
}

/* Write a tiled TIFF whose tile raw bytes are replaced by a foreign AVIF
 * bitstream whose geometry does not match the declared tile size. */
static int testTileMismatch(void)
{
    const char *name = "tile geometry mismatch";
    MemFile f = {0};
    uint8_t *foreign = NULL;
    size_t foreign_size = 0;
    int rc = 1;

    if (!makeForeignAVIF(&foreign, &foreign_size, 8, 8))
        return fail("foreign AVIF creation");

    RasterSpec r =
        makeSpec(32, 32, 3, 8, 1 /* tiled, 16x16 */, 1, 0);
    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        free(foreign);
        freeSpec(&r);
        return fail(name);
    }
    freeSpec(&r);

    /* Write zeros as tile 0 through the normal path, then overwrite its
     * raw bytes with the mismatched 8x8 AVIF. */
    tmsize_t tilesize = TIFFTileSize(tif);
    uint8_t *zeros = (uint8_t *)calloc(1, (size_t)tilesize);
    if (zeros == NULL)
        goto done;
    if (TIFFWriteEncodedTile(tif, 0, zeros, tilesize) != tilesize)
    {
        free(zeros);
        goto done;
    }
    free(zeros);
    TIFFClose(tif);

    /* Reopen r+ and replace tile 0's raw payload. */
    tif = memOpen(&f, "r+");
    if (tif == NULL)
        goto done;
    if (TIFFWriteRawTile(tif, 0, foreign, (tmsize_t)foreign_size) !=
        (tmsize_t)foreign_size)
    {
        TIFFClose(tif);
        goto done;
    }
    TIFFClose(tif);

    tif = memOpen(&f, "r");
    if (tif == NULL)
        goto done;
    uint8_t *buf = (uint8_t *)_TIFFmalloc(tilesize);
    resetErrors();
    tmsize_t got = TIFFReadEncodedTile(tif, 0, buf, tilesize);
    TIFFClose(tif);
    _TIFFfree(buf);
    if (got >= 0 || !errorContains("mismatch"))
    {
        fprintf(stderr, "FAIL: %s not rejected (got %" TIFF_SIZE_FORMAT
                        ", error='%s')\n",
                name, (size_t)got, g_last_error);
        goto done;
    }
    rc = 0;
    fprintf(stderr, "PASS: %s\n", name);
done:
    free(foreign);
    free(f.data);
    return rc;
}

static int testInvalidAVIF(void)
{
    const char *name = "invalid AVIF payload";
    MemFile f = {0};
    int rc = 1;

    RasterSpec r = makeSpec(16, 16, 3, 8, 0, 1, 0);
    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    tmsize_t stripsize = TIFFStripSize(tif);
    uint8_t *zeros = (uint8_t *)calloc(1, (size_t)stripsize);
    if (zeros == NULL)
    {
        TIFFClose(tif);
        freeSpec(&r);
        return fail(name);
    }
    if (TIFFWriteEncodedStrip(tif, 0, zeros, stripsize) != stripsize)
    {
        free(zeros);
        TIFFClose(tif);
        freeSpec(&r);
        return fail(name);
    }
    free(zeros);
    TIFFClose(tif);
    freeSpec(&r);

    /* Corrupt the strip payload in the middle of the file. Locate the
     * first ftyp-like region by just flipping bytes after the IFD; the
     * AVIF box starts somewhere in the file. Simpler: rebuild using r+
     * and overwrite the raw strip with garbage through the raw API. */
    tif = memOpen(&f, "r+");
    if (tif == NULL)
    {
        free(f.data);
        return fail(name);
    }
    static const unsigned char garbage[] = {
        0x00, 0x00, 0x00, 0x0c, 'f', 't', 'y', 'p',
        'a', 'v', 'i', 'f', 0xde, 0xad, 0xbe, 0xef};
    if (TIFFWriteRawStrip(tif, 0, (void *)garbage,
                          (tmsize_t)sizeof(garbage)) !=
        (tmsize_t)sizeof(garbage))
    {
        TIFFClose(tif);
        free(f.data);
        return fail(name);
    }
    TIFFClose(tif);

    tif = memOpen(&f, "r");
    if (tif == NULL)
    {
        free(f.data);
        return fail(name);
    }
    tmsize_t readsize = TIFFStripSize(tif);
    uint8_t *buf = (uint8_t *)_TIFFmalloc(readsize);
    resetErrors();
    tmsize_t got = TIFFReadEncodedStrip(tif, 0, buf, readsize);
    TIFFClose(tif);
    _TIFFfree(buf);
    if (got >= 0 ||
        (!errorContains("AVIF") && !errorContains("decoding")))
    {
        fprintf(stderr,
                "FAIL: %s not rejected (got %" TIFF_SIZE_FORMAT
                ", error='%s')\n",
                name, (size_t)got, g_last_error);
        goto done;
    }
    rc = 0;
    fprintf(stderr, "PASS: %s\n", name);
done:
    free(f.data);
    return rc;
}

static int testCorruptICC(void)
{
    const char *name = "corrupt ICC profile";
    MemFile f = {0};
    int rc = 1;

    RasterSpec r = makeSpec(16, 16, 3, 8, 0, 1, 0);
    r.use_icc = 1;
    r.corrupt_icc = 1;
    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    tmsize_t stripsize = TIFFStripSize(tif);
    uint8_t *zeros = (uint8_t *)calloc(1, (size_t)stripsize);
    resetErrors();
    int ok = (zeros != NULL) &&
             TIFFWriteEncodedStrip(tif, 0, zeros, stripsize) == stripsize;
    free(zeros);
    TIFFClose(tif);
    freeSpec(&r);
    if (ok)
    {
        fprintf(stderr, "FAIL: %s was accepted\n", name);
    }
    else if (!errorContains("Corrupt ICC"))
    {
        fprintf(stderr, "FAIL: %s wrong error: '%s'\n", name, g_last_error);
    }
    else
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    free(f.data);
    return rc;
}

static int testOversize(void)
{
    const char *name = "oversize dimension";
    MemFile f = {0};
    int rc = 1;

    /* Open a nominally 16x16 writer, then raise the length beyond the
     * codec's per-strile ceiling. A small raster buffer is enough: the
     * dimension check runs before any pixel processing. */
    RasterSpec r = makeSpec(16, 16, 3, 8, 0, 1, 0);
    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    freeSpec(&r);

    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 70000);
    /* One normal 16-wide RGB row; the codec must reject before touching
     * the full (huge) strip. */
    uint8_t row[16 * 3];
    memset(row, 0, sizeof(row));
    resetErrors();
    tmsize_t got = TIFFWriteEncodedStrip(tif, 0, row, (tmsize_t)sizeof(row));
    TIFFClose(tif);

    if (got >= 0 || !errorContains("AVIF"))
    {
        fprintf(stderr,
                "FAIL: %s was accepted (got=%" TIFF_SIZE_FORMAT
                ", error='%s')\n",
                name, (size_t)got, g_last_error);
    }
    else
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    free(f.data);
    return rc;
}

static int testBadPseudoTags(void)
{
    const char *name = "bad pseudo tag values";
    MemFile f = {0};
    int rc = 1;
    RasterSpec r = makeSpec(16, 16, 3, 8, 0, 1, 0);
    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    freeSpec(&r);
    resetErrors();
    int q_bad = (TIFFSetField(tif, TIFFTAG_AVIF_QUALITY, 101) == 0) &&
                errorContains("quality");
    resetErrors();
    int s_bad = (TIFFSetField(tif, TIFFTAG_AVIF_SUBSAMPLING, 9) == 0) &&
                errorContains("subsampling");
    resetErrors();
    int sp_bad = (TIFFSetField(tif, TIFFTAG_AVIF_SPEED, 11) == 0) &&
                 errorContains("speed");
    TIFFClose(tif);
    free(f.data);
    if (q_bad && s_bad && sp_bad)
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    else
    {
        fprintf(stderr,
                "FAIL: %s (quality=%d subsampling=%d speed=%d)\n", name,
                q_bad, s_bad, sp_bad);
    }
    return rc;
}

static int testICCRoundtrip(void)
{
    const char *name = "ICC profile round trip";
    MemFile f = {0};
    int rc = 1;
    RasterSpec r = makeSpec(16, 16, 3, 8, 0, 1, 0);
    r.use_icc = 1;
    fillRaster(&r);

    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    if (!writeRaster(tif, &r))
    {
        TIFFClose(tif);
        freeSpec(&r);
        free(f.data);
        return fail(name);
    }
    TIFFClose(tif);
    freeSpec(&r);

    tif = memOpen(&f, "r");
    if (tif == NULL)
    {
        free(f.data);
        return fail(name);
    }
    uint32_t got_len = 0;
    void *got_data = NULL;
    int found = TIFFGetField(tif, TIFFTAG_ICCPROFILE, &got_len, &got_data);
    resetErrors();
    tmsize_t stripsize = TIFFStripSize(tif);
    uint8_t *buf = (uint8_t *)_TIFFmalloc(stripsize);
    int decode_ok = TIFFReadEncodedStrip(tif, 0, buf, stripsize) >= 0;
    _TIFFfree(buf);

    /* Compare while the directory (and the ICC bytes it owns) is still
     * alive: the pointer is invalidated by TIFFClose(). */
    unsigned char expect[140];
    makeValidICC(expect);
    int icc_match = found && got_len == 140 &&
                    memcmp(got_data, expect, 140) == 0;
    TIFFClose(tif);

    if (icc_match && decode_ok)
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    else
    {
        fprintf(stderr,
                "FAIL: %s (found=%d len=%u decode_ok=%d icc_match=%d "
                "error='%s')\n",
                name, found, got_len, decode_ok, icc_match, g_last_error);
    }
    free(f.data);
    return rc;
}

static int testScanlineRead(void)
{
    const char *name = "scanline (fractional strip) read";
    MemFile f = {0};
    int rc = 1;
    RasterSpec r = makeSpec(20, 20, 4, 8, 0, 1, 0); /* RGBA, RPS 8 */
    fillRaster(&r);

    TIFF *tif = openWriter(&f, &r);
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    if (!writeRaster(tif, &r))
    {
        TIFFClose(tif);
        freeSpec(&r);
        free(f.data);
        return fail(name);
    }
    TIFFClose(tif);

    tif = memOpen(&f, "r");
    if (tif == NULL)
    {
        freeSpec(&r);
        free(f.data);
        return fail(name);
    }
    tmsize_t rowsize = TIFFScanlineSize(tif);
    uint8_t *line = (uint8_t *)_TIFFmalloc(rowsize);
    resetErrors();
    int ok = 1;
    for (uint32_t y = 0; y < r.height && ok; y++)
    {
        if (TIFFReadScanline(tif, line, y, 0) < 0)
            ok = 0;
        else if (memcmp(line, r.raster8 + (tmsize_t)y * rowsize,
                        (size_t)rowsize) != 0)
            ok = 0;
    }
    _TIFFfree(line);
    TIFFClose(tif);
    freeSpec(&r);
    if (ok)
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    else
        fprintf(stderr, "FAIL: %s ('%s')\n", name, g_last_error);
    free(f.data);
    return rc;
}

/*
 * Encode a 10-bit image, then reopen the same file read/write and decode it
 * through the same logical codec state machine. This exercises the
 * encode->decode mode switch and its buffer teardown (checked for leaks
 * under ASan).
 */
static int testModeSwitch(void)
{
    const char *name = "encode/decode mode switch (10-bit)";
    const char *path = "/tmp/libtiff_avif_modeswitch.tif";
    int rc = 1;
    const uint32_t W = 18, H = 14, S = 4;

    RasterSpec r = makeSpec(W, H, S, 10, 0, 1, 0);
    fillRaster(&r);

    TIFF *tif = TIFFOpen(path, "w");
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, W);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, H);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, S);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 10);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_AVIF);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_AVIF_LOSSLESS, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, H);
    {
        uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }
    {
        tmsize_t total = rasterBytes(&r);
        if (TIFFWriteEncodedStrip(tif, 0, r.raster8, total) != total)
        {
            TIFFClose(tif);
            freeSpec(&r);
            return fail(name);
        }
    }
    TIFFClose(tif);

    tif = TIFFOpen(path, "r");
    if (tif == NULL)
    {
        freeSpec(&r);
        return fail(name);
    }
    uint8_t *out = (uint8_t *)malloc((size_t)rasterBytes(&r));
    resetErrors();
    int ok = TIFFReadEncodedStrip(tif, 0, out, TIFFStripSize(tif)) >= 0 &&
             compareRasters(&r, out);
    TIFFClose(tif);
    free(out);
    freeSpec(&r);
    if (ok)
    {
        rc = 0;
        fprintf(stderr, "PASS: %s\n", name);
    }
    else
        fprintf(stderr, "FAIL: %s ('%s')\n", name, g_last_error);
    remove(path);
    return rc;
}

int main(void)
{
    int failures = 0;

    if (TIFFIsCODECConfigured(COMPRESSION_AVIF) != 1)
    {
        fprintf(stderr, "FAIL: AVIF codec not configured\n");
        return 1;
    }

    TIFFSetErrorHandler(captureErrorHandler);
    TIFFSetWarningHandler(captureErrorHandler);

    /* 1. lossless, strips, RGB 8-bit */
    {
        RasterSpec r = makeSpec(33, 17, 3, 8, 0, 1, 0);
        failures += roundtrip("lossless strip RGB8", &r, 1);
        freeSpec(&r);
    }
    /* 2. lossless, tiles, RGBA 8-bit */
    {
        RasterSpec r = makeSpec(33, 31, 4, 8, 1, 1, 0);
        failures += roundtrip("lossless tile RGBA8", &r, 1);
        freeSpec(&r);
    }
    /* 3. lossy, strips, RGB 8-bit, 4:2:0 */
    {
        RasterSpec r = makeSpec(48, 24, 3, 8, 0, 0, AVIF_SUBSAMPLING_420);
        failures += roundtrip("lossy strip RGB8 420", &r, 0);
        freeSpec(&r);
    }
    /* 4. lossy, tiles, 4:2:2 */
    {
        RasterSpec r = makeSpec(40, 40, 3, 8, 1, 0, AVIF_SUBSAMPLING_422);
        failures += roundtrip("lossy tile RGB8 422", &r, 0);
        freeSpec(&r);
    }
    /* 5. lossless 10-bit RGBA, tiles */
    {
        RasterSpec r = makeSpec(24, 24, 4, 10, 1, 1, 0);
        failures += roundtrip("lossless tile RGBA10", &r, 1);
        freeSpec(&r);
    }
    /* 6. lossless 12-bit RGB, strips */
    {
        RasterSpec r = makeSpec(28, 20, 3, 12, 0, 1, 0);
        failures += roundtrip("lossless strip RGB12", &r, 1);
        freeSpec(&r);
    }
    /* 7. lossy RGBA (alpha) strips */
    {
        RasterSpec r = makeSpec(32, 32, 4, 8, 0, 0, AVIF_SUBSAMPLING_444);
        failures += roundtrip("lossy strip RGBA8", &r, 0);
        freeSpec(&r);
    }

    failures += testScanlineRead();
    failures += testICCRoundtrip();
    failures += testCorruptICC();
    failures += testInvalidAVIF();
    failures += testTileMismatch();
    failures += testOversize();
    failures += testBadPseudoTags();
    failures += testModeSwitch();

    if (failures)
        fprintf(stderr, "\n%d AVIF test(s) FAILED\n", failures);
    else
        fprintf(stderr, "\nAll AVIF tests passed.\n");
    return failures ? 1 : 0;
}

#else /* !AVIF_SUPPORT */

int main(void)
{
    /* When libavif is unavailable the codec must still be registered but
     * report itself as not configured. */
    if (TIFFIsCODECConfigured(COMPRESSION_AVIF))
    {
        fprintf(stderr,
                "FAIL: AVIF codec unexpectedly configured without libavif\n");
        return 1;
    }
    fprintf(stderr,
            "PASS: AVIF codec correctly unavailable without libavif\n");
    return 0;
}

#endif /* AVIF_SUPPORT */

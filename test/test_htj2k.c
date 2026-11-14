/*
 * Copyright (c) 2026, LibTIFF contributors
 *
 * Functional tests for the HTJ2K (High Throughput JPEG 2000 / OpenJPH)
 * codec: lossless and near-lossless round trips, strips and tiles,
 * grayscale and color images, separate planar configuration, signed
 * samples and diagnostic handling of corrupt or inconsistent input.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names
 * of Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE.
 */

#include "tiffio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        ++checks;                                                              \
        if (!(cond))                                                           \
        {                                                                      \
            ++failures;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                      \
    } while (0)

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint16_t spp;            /* samples per pixel */
    uint16_t bps;            /* bits per sample */
    uint16_t sampleformat;
    uint16_t photometric;
    uint16_t planarconfig;
    int tiled;
    uint32_t tilewidth;
    uint32_t tilelength;
    uint32_t rowsperstrip;
    int lossless;
    double qstep;
} TestConfig;

static uint16_t sample_bytes(const TestConfig *cfg)
{
    return (uint16_t)(cfg->bps / 8);
}

static tmsize_t image_size(const TestConfig *cfg)
{
    return (tmsize_t)cfg->width * cfg->height * cfg->spp *
           sample_bytes(cfg);
}

static void fill_image(uint8_t *buf, const TestConfig *cfg, uint32_t seed)
{
    uint32_t n = cfg->width * cfg->height * cfg->spp;
    if (cfg->bps == 8)
    {
        if (cfg->sampleformat == SAMPLEFORMAT_INT)
        {
            int8_t *p = (int8_t *)buf;
            for (uint32_t i = 0; i < n; ++i)
                p[i] = (int8_t)((i * 37 + seed * 11) & 0xff) - 128;
        }
        else
        {
            for (uint32_t i = 0; i < n; ++i)
                buf[i] = (uint8_t)((i * 37 + seed * 11) & 0xff);
        }
    }
    else if (cfg->bps == 16)
    {
        if (cfg->sampleformat == SAMPLEFORMAT_INT)
        {
            int16_t *p = (int16_t *)buf;
            for (uint32_t i = 0; i < n; ++i)
                p[i] = (int16_t)(((i * 655 + seed * 13) & 0xffff) - 32768);
        }
        else
        {
            uint16_t *p = (uint16_t *)buf;
            for (uint32_t i = 0; i < n; ++i)
                p[i] = (uint16_t)((i * 655 + seed * 13) & 0xffff);
        }
    }
}

/* Count samples whose difference exceeds the given tolerance. */
static uint64_t count_differences(const uint8_t *a, const uint8_t *b,
                                  const TestConfig *cfg, int max_diff)
{
    uint64_t n = (uint64_t)cfg->width * cfg->height * cfg->spp;
    uint64_t diffs = 0;
    if (cfg->bps == 8)
    {
        if (cfg->sampleformat == SAMPLEFORMAT_INT)
        {
            const int8_t *pa = (const int8_t *)a;
            const int8_t *pb = (const int8_t *)b;
            for (uint64_t i = 0; i < n; ++i)
                if (abs((int)pa[i] - (int)pb[i]) > max_diff)
                    ++diffs;
        }
        else
        {
            for (uint64_t i = 0; i < n; ++i)
                if (abs((int)a[i] - (int)b[i]) > max_diff)
                    ++diffs;
        }
    }
    else
    {
        if (cfg->sampleformat == SAMPLEFORMAT_INT)
        {
            const int16_t *pa = (const int16_t *)a;
            const int16_t *pb = (const int16_t *)b;
            for (uint64_t i = 0; i < n; ++i)
                if (abs((int)pa[i] - (int)pb[i]) > max_diff)
                {
                    if (diffs < 5)
                        printf("  diff at %llu: %d vs %d\n",
                               (unsigned long long)i, pa[i], pb[i]);
                    ++diffs;
                }
        }
        else
        {
            const uint16_t *pa = (const uint16_t *)a;
            const uint16_t *pb = (const uint16_t *)b;
            for (uint64_t i = 0; i < n; ++i)
                if (abs((int)pa[i] - (int)pb[i]) > max_diff)
                {
                    if (diffs < 5)
                        printf("  diff at %llu: %u vs %u\n",
                               (unsigned long long)i, pa[i], pb[i]);
                    ++diffs;
                }
        }
    }
    return diffs;
}

static int write_htj2k(const char *path, const uint8_t *pixels,
                       const TestConfig *cfg)
{
    TIFF *tif = TIFFOpen(path, "w8");
    if (!tif)
        return 0;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, cfg->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, cfg->height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, cfg->spp);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, cfg->bps);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, cfg->sampleformat);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, cfg->photometric);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, cfg->planarconfig);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_HTJ2K);
    TIFFSetField(tif, TIFFTAG_HTJ2K_LOSSLESS, cfg->lossless);
    TIFFSetField(tif, TIFFTAG_HTJ2K_NUMDECOMP, 2);
    if (!cfg->lossless)
        TIFFSetField(tif, TIFFTAG_HTJ2K_QSTEP, cfg->qstep);

    if (cfg->tiled)
    {
        if (TIFFSetField(tif, TIFFTAG_TILEWIDTH, cfg->tilewidth) != 1 ||
            TIFFSetField(tif, TIFFTAG_TILELENGTH, cfg->tilelength) != 1)
        {
            TIFFClose(tif);
            return 0;
        }
    }
    else
    {
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, cfg->rowsperstrip);
    }

    int ok = 1;
    tmsize_t strip_size = TIFFStripSize(tif);
    tmsize_t tile_size = TIFFTileSize(tif);
    if (cfg->tiled)
    {
        uint32_t ntiles_x = (cfg->width + cfg->tilewidth - 1) /
                            cfg->tilewidth;
        uint32_t ntiles_y = (cfg->height + cfg->tilelength - 1) /
                            cfg->tilelength;
        uint16_t sample_count =
            (cfg->planarconfig == PLANARCONFIG_CONTIG) ? 1 : cfg->spp;
        for (uint16_t sample = 0; sample < sample_count && ok; ++sample)
        {
            for (uint32_t ty = 0; ty < ntiles_y && ok; ++ty)
            {
                for (uint32_t tx = 0; tx < ntiles_x; ++tx)
                {
                    uint32_t tile = TIFFComputeTile(
                        tif, tx * cfg->tilewidth, ty * cfg->tilelength,
                        0, sample);
                    uint8_t *tmp = (uint8_t *)_TIFFmalloc(tile_size);
                    if (!tmp)
                    {
                        ok = 0;
                        break;
                    }
                    memset(tmp, 0, (size_t)tile_size);
                    /* Copy the (possibly partial) tile region out of the
                     * whole-image buffer. */
                    uint32_t x0 = tx * cfg->tilewidth;
                    uint32_t y0 = ty * cfg->tilelength;
                    uint32_t w = cfg->tilewidth;
                    uint32_t h = cfg->tilelength;
                    if (x0 + w > cfg->width)
                        w = cfg->width - x0;
                    if (y0 + h > cfg->height)
                        h = cfg->height - y0;
                    if (cfg->planarconfig == PLANARCONFIG_CONTIG)
                    {
                        tmsize_t img_row = (tmsize_t)cfg->width * cfg->spp *
                                           sample_bytes(cfg);
                        tmsize_t tile_row =
                            (tmsize_t)cfg->tilewidth * cfg->spp *
                            sample_bytes(cfg);
                        tmsize_t valid_row = (tmsize_t)w * cfg->spp *
                                             sample_bytes(cfg);
                        for (uint32_t y = 0; y < h; ++y)
                            memcpy(tmp + y * tile_row,
                                   pixels + ((y0 + y) * img_row) +
                                       (tmsize_t)x0 * cfg->spp *
                                           sample_bytes(cfg),
                                   (size_t)valid_row);
                    }
                    else
                    {
                        /* One sample plane per tile. */
                        tmsize_t img_row =
                            (tmsize_t)cfg->width * sample_bytes(cfg);
                        tmsize_t tile_row =
                            (tmsize_t)cfg->tilewidth * sample_bytes(cfg);
                        tmsize_t plane = (tmsize_t)cfg->width *
                                         cfg->height * sample_bytes(cfg);
                        tmsize_t valid_row =
                            (tmsize_t)w * sample_bytes(cfg);
                        for (uint32_t y = 0; y < h; ++y)
                            memcpy(tmp + y * tile_row,
                                   pixels + sample * plane +
                                       (y0 + y) * img_row +
                                       (tmsize_t)x0 * sample_bytes(cfg),
                                   (size_t)valid_row);
                    }
                    if (TIFFWriteEncodedTile(tif, tile, tmp, tile_size) < 0)
                        ok = 0;
                    _TIFFfree(tmp);
                }
            }
        }
    }
    else
    {
        uint32_t nstrips =
            (cfg->height + cfg->rowsperstrip - 1) / cfg->rowsperstrip;
        for (uint32_t s = 0; s < nstrips && ok; ++s)
        {
            tmsize_t needed = TIFFVStripSize(tif, cfg->rowsperstrip);
            if (needed > strip_size)
                strip_size = needed;
            uint32_t rows = cfg->rowsperstrip;
            if ((s + 1) * rows > cfg->height)
                rows = cfg->height - s * rows;
            tmsize_t n = TIFFVStripSize(tif, rows);
            if (cfg->planarconfig == PLANARCONFIG_CONTIG || cfg->spp == 1)
            {
                tmsize_t row = (tmsize_t)cfg->width * cfg->spp *
                               sample_bytes(cfg);
                if (TIFFWriteEncodedStrip(
                        tif, s,
                        (uint8_t *)pixels +
                            (tmsize_t)s * cfg->rowsperstrip * row,
                        n) < 0)
                    ok = 0;
            }
            else
            {
                /* SEPARATE: strile index = component * nstrips + strip. */
                tmsize_t plane = (tmsize_t)cfg->width * cfg->height *
                                 sample_bytes(cfg);
                for (uint16_t c = 0; c < cfg->spp && ok; ++c)
                {
                    if (TIFFWriteEncodedStrip(
                            tif, c * nstrips + s,
                            (uint8_t *)pixels + c * plane +
                                (tmsize_t)s * cfg->rowsperstrip *
                                    cfg->width * sample_bytes(cfg),
                            n) < 0)
                        ok = 0;
                }
            }
        }
    }

    TIFFClose(tif);
    return ok;
}

static int read_htj2k(const char *path, uint8_t *out, const TestConfig *cfg)
{
    TIFF *tif = TIFFOpen(path, "r");
    if (!tif)
        return 0;

    uint16_t compression = 0;
    TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
    if (compression != COMPRESSION_HTJ2K)
    {
        printf("  unexpected compression %u\n", compression);
        TIFFClose(tif);
        return 0;
    }

    int ok = 1;
    if (cfg->tiled)
    {
        tmsize_t tile_size = TIFFTileSize(tif);
        uint8_t *tmp = (uint8_t *)_TIFFmalloc(tile_size);
        uint32_t ntiles_x = (cfg->width + cfg->tilewidth - 1) /
                            cfg->tilewidth;
        uint32_t ntiles_y = (cfg->height + cfg->tilelength - 1) /
                            cfg->tilelength;
        uint16_t sample_count =
            (cfg->planarconfig == PLANARCONFIG_CONTIG) ? 1 : cfg->spp;
        for (uint16_t sample = 0; sample < sample_count && ok; ++sample)
        {
            for (uint32_t ty = 0; ty < ntiles_y && ok; ++ty)
            {
                for (uint32_t tx = 0; tx < ntiles_x; ++tx)
                {
                    uint32_t tile = TIFFComputeTile(
                        tif, tx * cfg->tilewidth, ty * cfg->tilelength, 0,
                        sample);
                    if (TIFFReadEncodedTile(tif, tile, tmp, tile_size) < 0)
                    {
                        ok = 0;
                        break;
                    }
                    uint32_t x0 = tx * cfg->tilewidth;
                    uint32_t y0 = ty * cfg->tilelength;
                    uint32_t w = cfg->tilewidth;
                    uint32_t h = cfg->tilelength;
                    if (x0 + w > cfg->width)
                        w = cfg->width - x0;
                    if (y0 + h > cfg->height)
                        h = cfg->height - y0;
                    if (cfg->planarconfig == PLANARCONFIG_CONTIG)
                    {
                        tmsize_t img_row = (tmsize_t)cfg->width * cfg->spp *
                                           sample_bytes(cfg);
                        tmsize_t tile_row =
                            (tmsize_t)cfg->tilewidth * cfg->spp *
                            sample_bytes(cfg);
                        tmsize_t valid_row = (tmsize_t)w * cfg->spp *
                                             sample_bytes(cfg);
                        for (uint32_t y = 0; y < h; ++y)
                            memcpy(out + (y0 + y) * img_row +
                                       (tmsize_t)x0 * cfg->spp *
                                           sample_bytes(cfg),
                                   tmp + y * tile_row, (size_t)valid_row);
                    }
                    else
                    {
                        /* One sample plane per tile. */
                        tmsize_t img_row =
                            (tmsize_t)cfg->width * sample_bytes(cfg);
                        tmsize_t tile_row =
                            (tmsize_t)cfg->tilewidth * sample_bytes(cfg);
                        tmsize_t plane = (tmsize_t)cfg->width *
                                         cfg->height * sample_bytes(cfg);
                        tmsize_t valid_row =
                            (tmsize_t)w * sample_bytes(cfg);
                        for (uint32_t y = 0; y < h; ++y)
                            memcpy(out + sample * plane +
                                       (y0 + y) * img_row +
                                       (tmsize_t)x0 * sample_bytes(cfg),
                                   tmp + y * tile_row, (size_t)valid_row);
                    }
                }
            }
        }
        _TIFFfree(tmp);
    }
    else
    {
        tmsize_t strip_size = TIFFStripSize(tif);
        uint8_t *tmp = (uint8_t *)_TIFFmalloc(strip_size);
        uint32_t nstrips =
            (cfg->height + cfg->rowsperstrip - 1) / cfg->rowsperstrip;
        for (uint32_t s = 0; s < nstrips && ok; ++s)
        {
            uint32_t rows = cfg->rowsperstrip;
            if ((s + 1) * rows > cfg->height)
                rows = cfg->height - s * rows;
            tmsize_t n = TIFFVStripSize(tif, rows);
            if (cfg->planarconfig == PLANARCONFIG_CONTIG || cfg->spp == 1)
            {
                if (TIFFReadEncodedStrip(tif, s, tmp, n) < 0)
                    ok = 0;
                else
                {
                    tmsize_t row = (tmsize_t)cfg->width * cfg->spp *
                                   sample_bytes(cfg);
                    memcpy(out + (tmsize_t)s * cfg->rowsperstrip * row, tmp,
                           (size_t)n);
                }
            }
            else
            {
                tmsize_t plane = (tmsize_t)cfg->width * cfg->height *
                                 sample_bytes(cfg);
                for (uint16_t c = 0; c < cfg->spp && ok; ++c)
                {
                    if (TIFFReadEncodedStrip(tif, c * nstrips + s, tmp,
                                             n) < 0)
                        ok = 0;
                    else
                        memcpy(out + c * plane +
                                   (tmsize_t)s * cfg->rowsperstrip *
                                       cfg->width * sample_bytes(cfg),
                               tmp, (size_t)n);
                }
            }
        }
        _TIFFfree(tmp);
    }

    TIFFClose(tif);
    return ok;
}

static int run_roundtrip(const char *path, const TestConfig *cfg,
                         int tolerance, int require_lossy,
                         const char *label)
{
    tmsize_t sz = image_size(cfg);
    uint8_t *src = (uint8_t *)_TIFFmalloc(sz);
    uint8_t *dst = (uint8_t *)_TIFFmalloc(sz);
    if (!src || !dst)
    {
        printf("FAIL [%s]: out of memory\n", label);
        ++failures;
        _TIFFfree(src);
        _TIFFfree(dst);
        return 0;
    }
    fill_image(src, cfg, 7);
    memset(dst, 0xAB, (size_t)sz);

    int wok = write_htj2k(path, src, cfg);
    CHECK(wok, "[%s] write failed", label);
    int rok = 0;
    uint64_t diffs = 1;
    uint64_t any_diff = 0;
    if (wok)
    {
        rok = read_htj2k(path, dst, cfg);
        CHECK(rok, "[%s] read failed", label);
        if (rok)
        {
            diffs = count_differences(src, dst, cfg, tolerance);
            CHECK(diffs == 0,
                  "[%s] %llu sample(s) differ beyond tolerance %d", label,
                  (unsigned long long)diffs, tolerance);
            if (require_lossy)
            {
                any_diff = count_differences(src, dst, cfg, 0);
                CHECK(any_diff > 0,
                      "[%s] expected near-lossless changes but data is "
                      "identical",
                      label);
            }
        }
    }

    if (wok && rok && diffs == 0 &&
        (!require_lossy || any_diff > 0))
        printf("PASS [%s]\n", label);

    _TIFFfree(src);
    _TIFFfree(dst);
    return wok && rok && diffs == 0;
}

/* Corrupt the HTJ2K codestream of the first strip/tile in place.  The file
 * is reopened read-write and the SIZ marker segment (right after the SOC
 * marker) is destroyed, which must make header parsing fail. */
static int corrupt_first_strile(const char *path, int tiled)
{
    TIFF *tif = TIFFOpen(path, "r+");
    if (!tif)
        return 0;

    uint64_t *byte_counts = NULL;
    if (TIFFGetField(tif, tiled ? TIFFTAG_TILEBYTECOUNTS
                                : TIFFTAG_STRIPBYTECOUNTS,
                     &byte_counts) != 1 ||
        byte_counts == NULL || byte_counts[0] < 8)
    {
        TIFFClose(tif);
        return 0;
    }
    tmsize_t raw_size = (tmsize_t)byte_counts[0];
    uint8_t *stream = (uint8_t *)_TIFFmalloc(raw_size);
    if (!stream)
    {
        TIFFClose(tif);
        return 0;
    }
    tmsize_t got = tiled ? TIFFReadRawTile(tif, 0, stream, raw_size)
                         : TIFFReadRawStrip(tif, 0, stream, raw_size);
    if (got != raw_size || raw_size < 8)
    {
        _TIFFfree(stream);
        TIFFClose(tif);
        return 0;
    }
    /* Flip bytes in the SIZ marker segment (SOC occupies the first two
     * bytes). */
    for (int i = 2; i < 8 && i < (int)raw_size; ++i)
        stream[i] ^= (uint8_t)(0x5A + (unsigned)i);
    tmsize_t put = tiled ? TIFFWriteRawTile(tif, 0, stream, raw_size)
                         : TIFFWriteRawStrip(tif, 0, stream, raw_size);
    _TIFFfree(stream);
    TIFFClose(tif);
    return put == raw_size;
}

static void test_corrupt_input(void)
{
    const char *path = "htj2k_corrupt.tif";
    TestConfig cfg = {32, 24, 3, 8, SAMPLEFORMAT_UINT, PHOTOMETRIC_RGB,
                      PLANARCONFIG_CONTIG, 0, 0, 0, 8, 1, 0.0};
    tmsize_t sz = image_size(&cfg);
    uint8_t *src = (uint8_t *)_TIFFmalloc(sz);
    uint8_t *dst = (uint8_t *)_TIFFmalloc(sz);
    fill_image(src, &cfg, 3);

    int wok = write_htj2k(path, src, &cfg);
    CHECK(wok, "corrupt: write failed");
    if (wok)
    {
        int cok = corrupt_first_strile(path, 0);
        CHECK(cok, "corrupt: could not modify file");
        if (cok)
        {
            /* The codec must diagnose the bad codestream rather than crash
             * or silently return garbage. */
            TIFFSetWarningHandler(NULL);
            int rok = read_htj2k(path, dst, &cfg);
            CHECK(!rok,
                  "corrupt: decoding unexpectedly succeeded on bad data");
        }
    }
    _TIFFfree(src);
    _TIFFfree(dst);
    remove(path);
}

static void test_truncated_input(void)
{
    const char *src_path = "htj2k_trunc_src.tif";
    const char *trunc_path = "htj2k_trunc.tif";
    TestConfig cfg = {24, 16, 1, 8, SAMPLEFORMAT_UINT, PHOTOMETRIC_MINISBLACK,
                      PLANARCONFIG_CONTIG, 0, 0, 0, 8, 1, 0.0};
    tmsize_t sz = image_size(&cfg);
    uint8_t *src = (uint8_t *)_TIFFmalloc(sz);
    uint8_t *dst = (uint8_t *)_TIFFmalloc(sz);
    fill_image(src, &cfg, 11);

    int wok = write_htj2k(src_path, src, &cfg);
    CHECK(wok, "truncated: write failed");
    if (wok)
    {
        FILE *in = fopen(src_path, "rb");
        FILE *out = fopen(trunc_path, "wb");
        long fsize = 0;
        if (in)
        {
            fseek(in, 0, SEEK_END);
            fsize = ftell(in);
            rewind(in);
        }
        if (in && out && fsize > 8)
        {
            /* Chop the last quarter of the file off. */
            long keep = fsize - fsize / 4;
            char block[512];
            long remaining = keep;
            while (remaining > 0)
            {
                long n = remaining > (long)sizeof(block) ? (long)sizeof(block)
                                                          : remaining;
                size_t rd = fread(block, 1, (size_t)n, in);
                if (rd == 0)
                    break;
                fwrite(block, 1, rd, out);
                remaining -= (long)rd;
            }
        }
        if (in)
            fclose(in);
        if (out)
            fclose(out);

        TIFFSetWarningHandler(NULL);
        int rok = read_htj2k(trunc_path, dst, &cfg);
        CHECK(!rok, "truncated: decoding unexpectedly succeeded");
        remove(trunc_path);
    }
    _TIFFfree(src);
    _TIFFfree(dst);
    remove(src_path);
}

/* Geometry mismatch: replace the codestream of a small image with one
 * encoded for different dimensions and verify the codec rejects it. */
static void test_dimension_mismatch(void)
{
    const char *path_a = "htj2k_geom_a.tif";
    const char *path_b = "htj2k_geom_b.tif";
    TestConfig small = {16, 16, 1, 8, SAMPLEFORMAT_UINT,
                        PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 0, 0, 0,
                        16, 1, 0.0};
    TestConfig big = {32, 16, 1, 8, SAMPLEFORMAT_UINT,
                      PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 0, 0, 0,
                      16, 1, 0.0};
    tmsize_t sz_b = image_size(&big);
    tmsize_t sz_s = image_size(&small);
    uint8_t *pix_b = (uint8_t *)_TIFFmalloc(sz_b);
    uint8_t *pix_s = (uint8_t *)_TIFFmalloc(sz_s);
    uint8_t *out = (uint8_t *)_TIFFmalloc(sz_s);
    fill_image(pix_b, &big, 5);
    fill_image(pix_s, &small, 9);

    int wok_a = write_htj2k(path_a, pix_s, &small);
    int wok_b = write_htj2k(path_b, pix_b, &big);
    CHECK(wok_a && wok_b, "mismatch: setup writes failed");
    if (wok_a && wok_b)
    {
        /* Read the strip offsets/byte counts from both files and splice B's
         * codestream into A. */
        TIFF *ta = TIFFOpen(path_a, "r+");
        TIFF *tb = TIFFOpen(path_b, "r");
        if (ta && tb)
        {
            tmsize_t bsize = TIFFRawStripSize(tb, 0);
            tmsize_t asize = TIFFRawStripSize(ta, 0);
            uint8_t *bstream = (uint8_t *)_TIFFmalloc(bsize);
            if (bstream && TIFFReadRawStrip(tb, 0, bstream, bsize) ==
                               bsize)
            {
                if (bsize <= asize)
                {
                    TIFFSetWarningHandler(NULL);
                    TIFFSetErrorHandler(NULL);
                    tmsize_t wr =
                        TIFFWriteRawStrip(ta, 0, bstream, bsize);
                    CHECK(wr == bsize, "mismatch: splice write failed");
                    TIFFClose(ta);
                    ta = NULL;
                    int rok = read_htj2k(path_a, out, &small);
                    CHECK(!rok,
                          "mismatch: decoding unexpectedly succeeded");
                }
            }
            _TIFFfree(bstream);
        }
        if (ta)
            TIFFClose(ta);
        if (tb)
            TIFFClose(tb);
    }
    _TIFFfree(pix_b);
    _TIFFfree(pix_s);
    _TIFFfree(out);
    remove(path_a);
    remove(path_b);
}

static void test_invalid_parameters(void)
{
    const char *path = "htj2k_invalid.tif";

    /* Unsupported bit depth (24-bit) must fail at write setup. */
    {
        TIFF *tif = TIFFOpen(path, "w8");
        CHECK(tif != NULL, "invalid: open failed");
        if (tif)
        {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 24);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_HTJ2K);
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
            uint8_t line[16 * 3 * 3];
            memset(line, 0, sizeof(line));
            int rc = TIFFWriteEncodedStrip(tif, 0, line, sizeof(line));
            CHECK(rc < 0, "invalid: 24-bit write should fail");
            TIFFClose(tif);
        }
        remove(path);
    }

    /* Out-of-range pseudo tags. */
    {
        TIFF *tif = TIFFOpen(path, "w8");
        if (tif)
        {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 8);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 8);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_HTJ2K);
            int rc1 = TIFFSetField(tif, TIFFTAG_HTJ2K_NUMDECOMP, 99);
            CHECK(rc1 == 0, "invalid: bad decomposition count accepted");
            int rc2 = TIFFSetField(tif, TIFFTAG_HTJ2K_QSTEP, 12.0);
            CHECK(rc2 == 0, "invalid: bad quantization step accepted");

            /* Getters reflect configured values. */
            TIFFSetField(tif, TIFFTAG_HTJ2K_LOSSLESS, 0);
            int lossless = 1;
            TIFFGetField(tif, TIFFTAG_HTJ2K_LOSSLESS, &lossless);
            CHECK(lossless == 0, "invalid: lossless getter failed");
            TIFFClose(tif);
        }
        remove(path);
    }
}

/* Verify the codec name is registered and enumeration works. */
static void test_registration(void)
{
    const TIFFCodec *c = TIFFFindCODEC(COMPRESSION_HTJ2K);
    CHECK(c != NULL, "registration: codec not found");
    if (c)
        CHECK(strcmp(c->name, "HTJ2K") == 0,
              "registration: name is '%s'", c->name ? c->name : "(null)");
}

/* Chroma subsampling (2x2) in SEPARATE planar layout: luma must round trip
 * exactly; the chroma planes are downsampled, so only the samples at the
 * subsampled positions must match.  Covers both strips and tiles, with an
 * odd image size so the final regions are partial. */
static void test_subsampling(int tiled)
{
    const char *path = tiled ? "htj2k_subs_tile.tif" : "htj2k_subs_strip.tif";
    const uint32_t W = 17, H = 13, SPP = 3;
    const uint16_t DSX = 2, DSY = 2;
    const uint32_t TW = 16, TH = 16, RPS = 8;
    tmsize_t plane = (tmsize_t)W * H;
    tmsize_t total = plane * SPP;
    uint8_t *src = (uint8_t *)_TIFFmalloc(total);
    uint8_t *dst = (uint8_t *)_TIFFmalloc(total);
    fill_image(src,
               &(TestConfig){W, H, SPP, 8, SAMPLEFORMAT_UINT,
                             PHOTOMETRIC_YCBCR, PLANARCONFIG_SEPARATE,
                             tiled, TW, TH, RPS, 1, 0.0},
               17);
    memset(dst, 0xCD, (size_t)total);

    TIFF *tif = TIFFOpen(path, "w8");
    int ok = (tif != NULL);
    if (tif)
    {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, W);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, H);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, SPP);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_HTJ2K);
        TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, DSX, DSY);
        if (tiled)
        {
            TIFFSetField(tif, TIFFTAG_TILEWIDTH, TW);
            TIFFSetField(tif, TIFFTAG_TILELENGTH, TH);
        }
        else
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, RPS);

        if (tiled)
        {
            tmsize_t ts = TIFFTileSize(tif);
            uint8_t *tmp = (uint8_t *)_TIFFmalloc(ts);
            uint32_t ntx = (W + TW - 1) / TW;
            uint32_t nty = (H + TH - 1) / TH;
            for (uint16_t c = 0; c < SPP && ok; ++c)
                for (uint32_t ty = 0; ty < nty && ok; ++ty)
                    for (uint32_t tx = 0; tx < ntx; ++tx)
                    {
                        memset(tmp, 0, (size_t)ts);
                        uint32_t x0 = tx * TW, y0 = ty * TH;
                        uint32_t w = (x0 + TW > W) ? W - x0 : TW;
                        uint32_t h = (y0 + TH > H) ? H - y0 : TH;
                        uint32_t sx = (c == 0) ? 1 : DSX;
                        uint32_t sy = (c == 0) ? 1 : DSY;
                        for (uint32_t y = 0; y < h; ++y)
                            for (uint32_t x = 0; x < w; ++x)
                                tmp[y * TW + x] =
                                    src[c * plane + (y0 + y) * W + x0 + x];
                        (void)sx;
                        (void)sy;
                        uint32_t tile = TIFFComputeTile(tif, x0, y0, 0, c);
                        if (TIFFWriteEncodedTile(tif, tile, tmp, ts) < 0)
                            ok = 0;
                    }
            _TIFFfree(tmp);
        }
        else
        {
            uint32_t nstrips = (H + RPS - 1) / RPS;
            for (uint16_t c = 0; c < SPP && ok; ++c)
            {
                for (uint32_t s = 0; s < nstrips; ++s)
                {
                    uint32_t rows = RPS;
                    if ((s + 1) * rows > H)
                        rows = H - s * rows;
                    tmsize_t n = TIFFVStripSize(tif, rows);
                    if (TIFFWriteEncodedStrip(
                            tif, c * nstrips + s,
                            src + c * plane + (tmsize_t)s * RPS * W, n) < 0)
                        ok = 0;
                }
            }
        }
        TIFFClose(tif);
    }
    CHECK(ok, "subsampling(%s): write failed", tiled ? "tile" : "strip");

    if (ok)
    {
        tif = TIFFOpen(path, "r");
        int rok = (tif != NULL);
        if (tif)
        {
            if (tiled)
            {
                tmsize_t ts = TIFFTileSize(tif);
                uint8_t *tmp = (uint8_t *)_TIFFmalloc(ts);
                uint32_t ntx = (W + TW - 1) / TW;
                uint32_t nty = (H + TH - 1) / TH;
                for (uint16_t c = 0; c < SPP && rok; ++c)
                    for (uint32_t ty = 0; ty < nty && rok; ++ty)
                        for (uint32_t tx = 0; tx < ntx; ++tx)
                        {
                            uint32_t x0 = tx * TW, y0 = ty * TH;
                            uint32_t tile = TIFFComputeTile(tif, x0, y0, 0, c);
                            if (TIFFReadEncodedTile(tif, tile, tmp, ts) < 0)
                            {
                                rok = 0;
                                break;
                            }
                            uint32_t w = (x0 + TW > W) ? W - x0 : TW;
                            uint32_t h = (y0 + TH > H) ? H - y0 : TH;
                            for (uint32_t y = 0; y < h; ++y)
                                for (uint32_t x = 0; x < w; ++x)
                                    dst[c * plane + (y0 + y) * W + x0 + x] =
                                        tmp[y * TW + x];
                        }
                _TIFFfree(tmp);
            }
            else
            {
                uint32_t nstrips = (H + RPS - 1) / RPS;
                for (uint16_t c = 0; c < SPP && rok; ++c)
                    for (uint32_t s = 0; s < nstrips; ++s)
                    {
                        uint32_t rows = RPS;
                        if ((s + 1) * rows > H)
                            rows = H - s * rows;
                        tmsize_t n = TIFFVStripSize(tif, rows);
                        if (TIFFReadEncodedStrip(
                                tif, c * nstrips + s,
                                dst + c * plane + (tmsize_t)s * RPS * W,
                                n) < 0)
                            rok = 0;
                    }
            }
            TIFFClose(tif);
        }
        CHECK(rok, "subsampling(%s): read failed", tiled ? "tile" : "strip");

        if (rok)
        {
            /* Luma is full resolution and must be bit-exact. */
            uint64_t luma_bad = 0;
            for (uint32_t i = 0; i < plane; ++i)
                if (src[i] != dst[i])
                    ++luma_bad;
            CHECK(luma_bad == 0,
                  "subsampling(%s): %llu luma samples changed",
                  tiled ? "tile" : "strip",
                  (unsigned long long)luma_bad);

            /* Chroma: only the downsampled lattice positions are carried. */
            uint64_t chroma_bad = 0, checked = 0;
            for (uint16_t c = 1; c < SPP; ++c)
                for (uint32_t y = 0; y < H; y += DSY)
                    for (uint32_t x = 0; x < W; x += DSX)
                    {
                        ++checked;
                        if (src[c * plane + y * W + x] !=
                            dst[c * plane + y * W + x])
                            ++chroma_bad;
                    }
            CHECK(chroma_bad == 0 && checked > 0,
                  "subsampling(%s): %llu of %llu chroma lattice samples "
                  "changed",
                  tiled ? "tile" : "strip",
                  (unsigned long long)chroma_bad,
                  (unsigned long long)checked);
            if (luma_bad == 0 && chroma_bad == 0)
                printf("PASS [subsampling 2x2 %s]\n",
                       tiled ? "tile" : "strip");
        }
    }

    _TIFFfree(src);
    _TIFFfree(dst);
    remove(path);
}

int main(void)
{
    test_registration();

    TestConfig gray8_strip = {37, 29, 1, 8, SAMPLEFORMAT_UINT,
                              PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 0,
                              0, 0, 11, 1, 0.0};
    TestConfig gray8_tile = {37, 29, 1, 8, SAMPLEFORMAT_UINT,
                             PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 1,
                             16, 16, 0, 1, 0.0};
    TestConfig rgb8_strip = {33, 27, 3, 8, SAMPLEFORMAT_UINT,
                             PHOTOMETRIC_RGB, PLANARCONFIG_CONTIG, 0, 0, 0,
                             9, 1, 0.0};
    TestConfig rgb8_tile = {33, 27, 3, 8, SAMPLEFORMAT_UINT,
                            PHOTOMETRIC_RGB, PLANARCONFIG_CONTIG, 1, 16, 16,
                            0, 1, 0.0};
    TestConfig rgb8_sep_strip = {33, 27, 3, 8, SAMPLEFORMAT_UINT,
                                 PHOTOMETRIC_RGB, PLANARCONFIG_SEPARATE, 0, 0,
                                 0, 9, 1, 0.0};
    TestConfig rgb8_sep_tile = {33, 27, 3, 8, SAMPLEFORMAT_UINT,
                                PHOTOMETRIC_RGB, PLANARCONFIG_SEPARATE, 1, 16,
                                16, 0, 1, 0.0};
    TestConfig gray16_strip = {31, 23, 1, 16, SAMPLEFORMAT_UINT,
                               PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 0,
                               0, 0, 7, 1, 0.0};
    TestConfig gray16s_tile = {31, 23, 1, 16, SAMPLEFORMAT_INT,
                               PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, 1,
                               16, 16, 0, 1, 0.0};
    TestConfig rgb16_strip = {29, 21, 3, 16, SAMPLEFORMAT_UINT,
                              PHOTOMETRIC_RGB, PLANARCONFIG_CONTIG, 0, 0, 0, 7,
                              1, 0.0};

    /* Lossless round trips (tolerance 0). */
    run_roundtrip("htj2k_gray8_strip.tif", &gray8_strip, 0, 0,
                  "lossless gray8 strip");
    run_roundtrip("htj2k_gray8_tile.tif", &gray8_tile, 0, 0,
                  "lossless gray8 tile");
    run_roundtrip("htj2k_rgb8_strip.tif", &rgb8_strip, 0, 0,
                  "lossless rgb8 strip");
    run_roundtrip("htj2k_rgb8_tile.tif", &rgb8_tile, 0, 0,
                  "lossless rgb8 tile");
    run_roundtrip("htj2k_rgb8_sep_strip.tif", &rgb8_sep_strip, 0, 0,
                  "lossless rgb8 separate strip");
    run_roundtrip("htj2k_rgb8_sep_tile.tif", &rgb8_sep_tile, 0, 0,
                  "lossless rgb8 separate tile");
    run_roundtrip("htj2k_gray16_strip.tif", &gray16_strip, 0, 0,
                  "lossless gray16 strip");
    run_roundtrip("htj2k_gray16s_tile.tif", &gray16s_tile, 0, 0,
                  "lossless gray16 signed tile");
    run_roundtrip("htj2k_rgb16_strip.tif", &rgb16_strip, 0, 0,
                  "lossless rgb16 strip");

    /* Near-lossless round trips: irreversible 9-7 wavelet with a finite
     * quantization step. Allow a bounded tolerance and verify the data
     * actually changed (the irreversible path is exercised). */
    TestConfig rgb8_lossy_strip = rgb8_strip;
    rgb8_lossy_strip.lossless = 0;
    rgb8_lossy_strip.qstep = 0.02;
    run_roundtrip("htj2k_rgb8_lossy_strip.tif", &rgb8_lossy_strip, 12, 1,
                  "near-lossless rgb8 strip");
    TestConfig gray16_lossy_tile = gray16_strip;
    gray16_lossy_tile.tiled = 1;
    gray16_lossy_tile.tilewidth = 16;
    gray16_lossy_tile.tilelength = 16;
    gray16_lossy_tile.lossless = 0;
    gray16_lossy_tile.qstep = 0.02;
    run_roundtrip("htj2k_gray16_lossy_tile.tif", &gray16_lossy_tile, 2000, 1,
                  "near-lossless gray16 tile");

    test_subsampling(0);
    test_subsampling(1);

    test_corrupt_input();
    test_truncated_input();
    test_dimension_mismatch();
    test_invalid_parameters();

    if (failures == 0)
        printf("\nAll HTJ2K tests passed (%d checks).\n", checks);
    else
        printf("\n%d HTJ2K test(s) FAILED (%d checks).\n", failures, checks);
    return failures ? 1 : 0;
}

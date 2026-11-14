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

#include "tiffiop.h"
#ifdef AVIF_SUPPORT
/*
 * TIFF Library.
 *
 * AVIF (AV1 Image File Format) Compression Support, implemented on top of
 * libavif (https://github.com/AOMediaCodec/libavif).
 *
 * COMPRESSION_AVIF (50003) is an experimental, privately allocated
 * compression code. It is not registered in the Adobe-maintained TIFF
 * compression registry.
 *
 * Design notes:
 *  - Each TIFF strip/tile is encoded as one independent AVIF bitstream.
 *    Encoding is strictly per-strile: the codec never accumulates data of
 *    more than one strile at a time, so the working set is bounded by the
 *    raw bytes of a single strile plus the (reused) compressed output
 *    buffer and the temporary YUV planes that libavif allocates for that
 *    one strile.
 *  - Supported rasters: PLANARCONFIG_CONTIG, 3 (RGB) or 4 (RGBA) samples
 *    per pixel, unsigned integer samples of 8, 10 or 12 bits. For the
 *    10/12-bit depths the TIFF raster is bit-packed (the same layout
 *    TIFFScanlineSize/TIFFTileSize describe); this codec unpacks it into
 *    the uint16_t containers libavif expects and re-packs on decode.
 *  - The ICC profile is carried in the regular TIFF ICCProfile tag (34675)
 *    and is additionally embedded inside the AVIF container so that a
 *    standalone .avif extraction stays color-managed.
 *  - On failure the codec always returns an error; it never silently falls
 *    back to uncompressed storage.
 */

#include <avif/avif.h>

#include <stdio.h>
#include <string.h>

#define LSTATE_INIT_DECODE 0x01
#define LSTATE_INIT_ENCODE 0x02

/*
 * AVIF (and the AV1 level-5.x constraints that matter for tiled imagery)
 * cannot address dimensions beyond 65535. Reject anything larger up front
 * with a distinct, actionable error instead of relying on the underlying
 * AV1 codec to fail.
 */
#define AVIF_MAX_DIMENSION 65535u

/*
 * State block for each open TIFF file using AVIF compression/decompression.
 */
typedef struct
{
    uint16_t nSamples;      /* samples per pixel (3 or 4) */
    uint16_t bitsPerSample; /* 8, 10 or 12 */

    int read_error; /* sticky per-strile decode failure */

    /* Encoder configuration (set through pseudo tags). */
    int quality;     /* 0..100, AVIF_QUALITY_LOSSLESS == 100 */
    int lossless;    /* boolean */
    int speed;       /* 0..10 */
    int subsampling; /* AVIF_SUBSAMPLING_444 / _422 / _420 */

    /* Strile accumulation buffer (raw raster bytes, reused across striles).
     * For 8-bit data this is also the libavif RGB pixel source. For 10/12-bit
     * data it holds the bit-packed TIFF raster, which is unpacked into
     * pPacked16 before being handed to libavif. */
    uint8_t *pBuffer;
    tmsize_t buffer_size;
    tmsize_t buffer_offset;

    /* 16-bit-container working buffer for 10/12-bit conversion. */
    uint16_t *pPacked16;
    tmsize_t packed16_size; /* in bytes */

    /* Decode-side scratch buffer, used only when the caller reads a
     * strile in fractional pieces (scanline reads). */
    uint8_t *pDecodeBuffer;
    tmsize_t decode_buffer_size;
    int last_y; /* number of rows already handed out of the decoded strile */
    int decode_height;

    int state; /* LSTATE_* flags */

    TIFFVGetMethod vgetparent; /* super-class method */
    TIFFVSetMethod vsetparent; /* super-class method */
} AVIFState;

#define LState(tif) ((AVIFState *)(tif)->tif_data)
#define DecoderState(tif) LState(tif)
#define EncoderState(tif) LState(tif)

static int TAVIFEncode(TIFF *tif, uint8_t *bp, tmsize_t cc, uint16_t s);
static int TAVIFDecode(TIFF *tif, uint8_t *op, tmsize_t occ, uint16_t s);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int TAVIFSupportedDepth(uint16_t bitsPerSample)
{
    return bitsPerSample == 8 || bitsPerSample == 10 ||
           bitsPerSample == 12;
}

static size_t TAVIFBytesPerSample(uint16_t bitsPerSample)
{
    /* libavif always addresses 10/12-bit samples through uint16_t
     * containers; the libtiff raster itself keeps those depths bit-packed.
     */
    return bitsPerSample <= 8 ? 1 : 2;
}

/*
 * Byte size of one raster row as libtiff lays it out. For depths above 8
 * the samples are bit-packed (TIFFTileRowSize/TIFFScanlineSize semantics),
 * so this cannot be derived from a per-sample byte size.
 */
static tmsize_t TAVIFRasterRowBytes(TIFF *tif, uint32_t width)
{
    TIFFDirectory *td = &tif->tif_dir;
    uint64_t bits = (uint64_t)width * (uint64_t)td->td_samplesperpixel *
                    (uint64_t)td->td_bitspersample;
    return (tmsize_t)((bits + 7u) / 8u);
}

/*
 * Total byte size of a strile in the libtiff raster layout.
 */
static tmsize_t TAVIFRasterStrileSize(TIFF *tif, uint32_t width,
                                      uint32_t height)
{
    return TAVIFRasterRowBytes(tif, width) * (tmsize_t)height;
}

/*
 * Unpack bit-packed samples of 10 or 12 bits into libavif's uint16_t
 * containers. srcRowBytes is the (possibly rounded up) packed row stride.
 */
static void TAVIFUnpack16(const uint8_t *src, uint16_t *dst,
                          uint32_t samples_per_row, uint16_t bits)
{
    uint64_t bitpos = 0;
    const uint32_t mask = (1u << bits) - 1u;

    for (uint32_t i = 0; i < samples_per_row; i++)
    {
        uint64_t byte_index = bitpos / 8u;
        uint32_t bit_offset = (uint32_t)(bitpos % 8u);
        uint32_t avail = 8u - bit_offset;
        uint32_t v;

        if (avail >= bits)
        {
            v = (src[byte_index] >> (avail - bits)) & mask;
        }
        else
        {
            v = ((uint32_t)src[byte_index] & ((1u << avail) - 1u))
                << (bits - avail);
            v |= (uint32_t)src[byte_index + 1] >> (8u - (bits - avail));
            v &= mask;
        }
        dst[i] = (uint16_t)v;
        bitpos += bits;
    }
}

/*
 * Pack libavif's uint16_t samples back into the libtiff bit-packed raster.
 * Pads the final partial byte of each row with zero bits, matching how
 * libtiff computes row sizes.
 */
static void TAVIFPack16(const uint16_t *src, uint8_t *dst,
                        uint32_t samples_per_row, uint16_t bits)
{
    uint64_t bitpos = 0;
    const uint32_t mask = (1u << bits) - 1u;

    for (uint32_t i = 0; i < samples_per_row; i++)
    {
        uint32_t v = (uint32_t)src[i] & mask;
        uint64_t byte_index = bitpos / 8u;
        uint32_t bit_offset = (uint32_t)(bitpos % 8u);
        uint32_t avail = 8u - bit_offset;

        if (avail >= bits)
        {
            dst[byte_index] |= (uint8_t)(v << (avail - bits));
        }
        else
        {
            dst[byte_index] |= (uint8_t)(v >> (bits - avail));
            dst[byte_index + 1] =
                (uint8_t)((v << (8u - (bits - avail))) & 0xffu);
        }
        bitpos += bits;
    }
}

/*
 * Compute the dimensions of the strile currently being processed. Mirrors
 * the logic used by the other whole-image codecs (WebP, JPEG): edge tiles
 * keep the declared tile geometry, whereas the last strip may be shorter.
 * The returned geometry is always that of the complete strile, even for
 * fractional (scanline) reads: a strip's AVIF bitstream always encodes the
 * whole strip, and per-row serving is handled by the decode scratch buffer.
 */
static void TAVIFGetStrileDimensions(TIFF *tif, uint32_t *width,
                                     uint32_t *height)
{
    TIFFDirectory *td = &tif->tif_dir;

    if (isTiled(tif))
    {
        *width = td->td_tilewidth;
        *height = td->td_tilelength;
    }
    else
    {
        /* td_row is the row currently being read/written, not the first
         * row of the strip. Derive the strip's first row from it. */
        uint32_t rps = td->td_rowsperstrip;
        uint32_t strip_first =
            rps ? (td->td_row / rps) * rps : 0;
        uint32_t h = td->td_imagelength - strip_first;
        if (rps && h > rps)
            h = rps;
        *width = td->td_imagewidth;
        *height = h;
    }
}

static avifPixelFormat TAVIFPixelFormat(int subsampling, int hasAlpha)
{
    (void)hasAlpha; /* alpha is always a separate, full-resolution plane */
    switch (subsampling)
    {
        case AVIF_SUBSAMPLING_422:
            return AVIF_PIXEL_FORMAT_YUV422;
        case AVIF_SUBSAMPLING_420:
            return AVIF_PIXEL_FORMAT_YUV420;
        case AVIF_SUBSAMPLING_444:
        default:
            return AVIF_PIXEL_FORMAT_YUV444;
    }
}

/*
 * Minimal structural sanity check of an ICC profile. libavif only stores
 * the profile, it does not validate it, so a corrupted profile would be
 * silently embedded otherwise. We verify the documented fixed layout:
 * total length, signature and the table counts declared in the header.
 * See ICC.1, clause 7.2 (profile header).
 */
static int TAVIFValidateICC(TIFF *tif, const uint8_t *icc,
                            tmsize_t icc_size)
{
    static const char module[] = "AVIFValidateICC";
    uint32_t profile_size;
    uint32_t tag_count;

    if (icc == NULL || icc_size < 132)
    {
        TIFFErrorExtR(tif, module,
                      "Corrupt ICC profile: too short (%" TIFF_SIZE_FORMAT
                      " bytes, 132-byte header required)",
                      (size_t)icc_size);
        return 0;
    }

    profile_size = ((uint32_t)icc[0] << 24) | ((uint32_t)icc[1] << 16) |
                   ((uint32_t)icc[2] << 8) | (uint32_t)icc[3];
    if (profile_size != (uint32_t)icc_size)
    {
        TIFFErrorExtR(tif, module,
                      "Corrupt ICC profile: declared size %" PRIu32
                      " does not match tag size %" TIFF_SIZE_FORMAT,
                      profile_size, (size_t)icc_size);
        return 0;
    }

    if (memcmp(icc + 36, "acsp", 4) != 0)
    {
        TIFFErrorExtR(tif, module,
                      "Corrupt ICC profile: missing 'acsp' signature");
        return 0;
    }

    tag_count = ((uint32_t)icc[128] << 24) | ((uint32_t)icc[129] << 16) |
                ((uint32_t)icc[130] << 8) | (uint32_t)icc[131];
    /* 4-byte signature + 4-byte tag count + 12 bytes per tag entry. */
    if ((uint64_t)132 + (uint64_t)tag_count * 12u > (uint64_t)icc_size)
    {
        TIFFErrorExtR(tif, module,
                      "Corrupt ICC profile: tag count %" PRIu32
                      " overflows the %" TIFF_SIZE_FORMAT "-byte profile",
                      tag_count, (size_t)icc_size);
        return 0;
    }

    return 1;
}

/*
 * Fetch the ICC profile held in the regular TIFF ICCProfile tag (34675).
 * Returns 1 and fills out icc/icc_size when present, 0 when absent.
 */
static int TAVIFGetTIFFICC(TIFF *tif, const uint8_t **icc,
                           uint32_t *icc_size)
{
    void *data = NULL;

    *icc = NULL;
    *icc_size = 0;
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, icc_size, &data))
    {
        *icc = (const uint8_t *)data;
        return 1;
    }
    return 0;
}

/* Translate libavif result codes into distinct, stable TIFF error messages. */
static void TAVIFReportResult(TIFF *tif, const char *module,
                              const char *action, avifResult res)
{
    TIFFErrorExtR(tif, module,
                  "AVIF %s failed: %s (avifResult=%d)", action,
                  avifResultToString(res), (int)res);
}

/* ------------------------------------------------------------------ */
/* Encoding                                                           */
/* ------------------------------------------------------------------ */

/*
 * Accumulate raw raster bytes for the strile currently being written.
 * The actual AVIF encoding happens in TAVIFPostEncode(), once the whole
 * strile has been received.
 */
static int TAVIFEncode(TIFF *tif, uint8_t *bp, tmsize_t cc, uint16_t s)
{
    static const char module[] = "TAVIFEncode";
    AVIFState *sp = EncoderState(tif);

    (void)s;
    assert(sp != NULL);
    assert(sp->state == LSTATE_INIT_ENCODE);

    if (cc < 0 || sp->buffer_offset + cc > sp->buffer_size)
    {
        TIFFErrorExtR(tif, module,
                      "Too many bytes for this AVIF strip/tile");
        return 0;
    }

    _TIFFmemcpy(sp->pBuffer + sp->buffer_offset, bp, (size_t)cc);
    sp->buffer_offset += cc;

    return 1;
}

static int TAVIFPostEncode(TIFF *tif)
{
    static const char module[] = "TAVIFPostEncode";
    AVIFState *sp = EncoderState(tif);
    uint32_t segment_width, segment_height;
    avifImage *image = NULL;
    avifEncoder *encoder = NULL;
    avifRGBImage rgb;
    avifRWData encoded = AVIF_DATA_EMPTY;
    avifResult res;
    avifPixelFormat pixel_format;
    int rc = 0;

    assert(sp != NULL);
    assert(sp->state == LSTATE_INIT_ENCODE);

    TAVIFGetStrileDimensions(tif, &segment_width, &segment_height);

    if (segment_width == 0 || segment_height == 0 ||
        segment_width > AVIF_MAX_DIMENSION ||
        segment_height > AVIF_MAX_DIMENSION)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF strip/tile dimension %" PRIu32 "x%" PRIu32
                      " is zero or exceeds the %" PRIu32
                      " pixel AVIF limit",
                      segment_width, segment_height, AVIF_MAX_DIMENSION);
        return 0;
    }

    /* Re-check the expected strile size against what was actually fed in. */
    {
        tmsize_t expected =
            TAVIFRasterStrileSize(tif, segment_width, segment_height);
        if (sp->buffer_offset != expected)
        {
            TIFFErrorExtR(tif, module,
                          "AVIF strile size mismatch: got %" TIFF_SIZE_FORMAT
                          " raw bytes, expected %" TIFF_SIZE_FORMAT,
                          (size_t)sp->buffer_offset, (size_t)expected);
            return 0;
        }
    }

    pixel_format = TAVIFPixelFormat(sp->subsampling, sp->nSamples == 4);

    image = avifImageCreate(segment_width, segment_height,
                            sp->bitsPerSample, pixel_format);
    if (image == NULL)
    {
        TIFFErrorExtR(tif, module,
                      "Cannot allocate AVIF image for strip/tile");
        goto cleanup;
    }
    /* Full range keeps the RGB<->YUV round trip numerically honest.
     * With 4:4:4 chroma and the identity matrix coefficient libavif keeps
     * the planes as plain GBR (no color matrix), which is what makes the
     * lossless mode genuinely sample-exact. For 4:2:2/4:2:0 an actual
     * chroma matrix is needed, so use BT.601. */
    image->yuvRange = AVIF_RANGE_FULL;
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT601;
    image->transferCharacteristics =
        AVIF_TRANSFER_CHARACTERISTICS_BT601;
    image->matrixCoefficients =
        (pixel_format == AVIF_PIXEL_FORMAT_YUV444)
            ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
            : AVIF_MATRIX_COEFFICIENTS_BT601;

    /* ICC profile: validate before embedding it into the AVIF container. */
    {
        const uint8_t *icc = NULL;
        uint32_t icc_size = 0;
        if (TAVIFGetTIFFICC(tif, &icc, &icc_size))
        {
            if (!TAVIFValidateICC(tif, icc, (tmsize_t)icc_size))
            {
                goto cleanup;
            }
            res = avifImageSetProfileICC(image, icc, icc_size);
            if (res != AVIF_RESULT_OK)
            {
                TAVIFReportResult(tif, module, "ICC profile embedding", res);
                goto cleanup;
            }
        }
    }

    /* Build the RGB image handed to libavif. For 8-bit depths the packed
     * raster accumulation buffer is wrapped directly. For 10/12-bit depths
     * the bit-packed TIFF raster is unpacked into uint16_t containers. */
    avifRGBImageSetDefaults(&rgb, image);
    rgb.width = segment_width;
    rgb.height = segment_height;
    rgb.depth = sp->bitsPerSample;
    rgb.format = (sp->nSamples == 4) ? AVIF_RGB_FORMAT_RGBA
                                     : AVIF_RGB_FORMAT_RGB;

    uint8_t *rgb_pixels = sp->pBuffer;
    uint32_t rgb_rowbytes =
        segment_width * sp->nSamples *
        (uint32_t)TAVIFBytesPerSample(sp->bitsPerSample);

    if (sp->bitsPerSample > 8)
    {
        tmsize_t needed16 =
            (tmsize_t)segment_width * (tmsize_t)segment_height *
            (tmsize_t)sp->nSamples * (tmsize_t)sizeof(uint16_t);
        if (needed16 > sp->packed16_size)
        {
            if (sp->pPacked16 != NULL)
                _TIFFfreeExt(tif, sp->pPacked16);
            sp->pPacked16 =
                (uint16_t *)_TIFFmallocExt(tif, needed16);
            if (sp->pPacked16 == NULL)
            {
                TIFFErrorExtR(tif, module,
                              "Cannot allocate 16-bit conversion buffer");
                sp->packed16_size = 0;
                goto cleanup;
            }
            sp->packed16_size = needed16;
        }
        tmsize_t src_rowbytes =
            TAVIFRasterRowBytes(tif, segment_width);
        uint32_t samples_per_row = segment_width * sp->nSamples;
        for (uint32_t y = 0; y < segment_height; y++)
        {
            TAVIFUnpack16(
                sp->pBuffer + (tmsize_t)y * src_rowbytes,
                sp->pPacked16 + (uint64_t)y * samples_per_row,
                samples_per_row, sp->bitsPerSample);
        }
        rgb_pixels = (uint8_t *)sp->pPacked16;
        rgb_rowbytes = samples_per_row * (uint32_t)sizeof(uint16_t);
    }

    rgb.pixels = rgb_pixels;
    rgb.rowBytes = rgb_rowbytes;

    res = avifImageRGBToYUV(image, &rgb);
    if (res != AVIF_RESULT_OK)
    {
        TAVIFReportResult(tif, module, "RGB to YUV conversion", res);
        goto cleanup;
    }

    encoder = avifEncoderCreate();
    if (encoder == NULL)
    {
        TIFFErrorExtR(tif, module, "Cannot allocate AVIF encoder");
        goto cleanup;
    }
    encoder->maxThreads = 1; /* keep peak memory deterministic */
    encoder->speed = sp->speed;
    if (sp->lossless)
    {
        encoder->quality = AVIF_QUALITY_LOSSLESS;
        encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    }
    else
    {
        encoder->quality = sp->quality;
        /* Encode alpha with the same quality as the color planes. */
        encoder->qualityAlpha = sp->quality;
    }

    res = avifEncoderWrite(encoder, image, &encoded);
    if (res != AVIF_RESULT_OK)
    {
        TAVIFReportResult(tif, module, "encoding", res);
        goto cleanup;
    }

    if (encoded.size == 0 || encoded.data == NULL)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF encoder produced an empty bitstream");
        goto cleanup;
    }

    /* Make sure the TIFF raw buffer can hold the compressed strile. The
     * framework grows it on demand; growing never changes the raster
     * accumulation buffer, which is owned by this codec. */
    if ((tmsize_t)encoded.size > tif->tif_rawdatasize)
    {
        tmsize_t needed = (tmsize_t)encoded.size;
        if (!TIFFWriteBufferSetup(tif, NULL, needed))
        {
            TIFFErrorExtR(tif, module,
                          "Cannot grow output buffer to %" TIFF_SIZE_FORMAT
                          " bytes",
                          (size_t)needed);
            goto cleanup;
        }
    }

    _TIFFmemcpy(tif->tif_rawcp, encoded.data, encoded.size);
    tif->tif_rawcc = (tmsize_t)encoded.size;

    if (!TIFFFlushData1(tif))
    {
        TIFFErrorExtR(tif, module, "Error flushing TIFF AVIF encoder");
        goto cleanup;
    }

    rc = 1;

cleanup:
    avifRWDataFree(&encoded);
    if (encoder != NULL)
        avifEncoderDestroy(encoder);
    if (image != NULL)
        avifImageDestroy(image);
    /* The accumulation buffer is reused for the next strile. */
    sp->buffer_offset = 0;
    return rc;
}static int TAVIFSetupEncode(TIFF *tif)
{
    static const char module[] = "TAVIFSetupEncode";
    AVIFState *sp = EncoderState(tif);
    uint16_t nBitsPerSample = tif->tif_dir.td_bitspersample;
    uint16_t sampleFormat = tif->tif_dir.td_sampleformat;

    assert(sp != NULL);

    sp->nSamples = tif->tif_dir.td_samplesperpixel;

    if (sp->nSamples != 3 && sp->nSamples != 4)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF driver doesn't support %d bands. Must be 3 (RGB) "
                      "or 4 (RGBA) bands.",
                      sp->nSamples);
        return 0;
    }

    if (!TAVIFSupportedDepth(nBitsPerSample) ||
        sampleFormat != SAMPLEFORMAT_UINT)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF driver requires unsigned integer data with 8, "
                      "10 or 12 bits per sample (got %u bits, sample format "
                      "%u)",
                      (unsigned)nBitsPerSample, (unsigned)sampleFormat);
        return 0;
    }
    sp->bitsPerSample = nBitsPerSample;

    if (tif->tif_dir.td_photometric != PHOTOMETRIC_RGB)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF driver requires PhotometricInterpretation=RGB");
        return 0;
    }

    if (sp->state & LSTATE_INIT_DECODE)
    {
        if (sp->pDecodeBuffer != NULL)
        {
            _TIFFfreeExt(tif, sp->pDecodeBuffer);
            sp->pDecodeBuffer = NULL;
            sp->decode_buffer_size = 0;
        }
        if (sp->pPacked16 != NULL)
        {
            _TIFFfreeExt(tif, sp->pPacked16);
            sp->pPacked16 = NULL;
            sp->packed16_size = 0;
        }
        sp->last_y = 0;
        sp->state = 0;
    }

    sp->state |= LSTATE_INIT_ENCODE;
    return 1;
}

static int TAVIFPreEncode(TIFF *tif, uint16_t s)
{
    static const char module[] = "TAVIFPreEncode";
    AVIFState *sp = EncoderState(tif);
    uint32_t segment_width, segment_height;
    tmsize_t strile_size;

    (void)s;
    assert(sp != NULL);
    if (sp->state != LSTATE_INIT_ENCODE)
        tif->tif_setupencode(tif);

    TAVIFGetStrileDimensions(tif, &segment_width, &segment_height);

    if (segment_width > AVIF_MAX_DIMENSION ||
        segment_height > AVIF_MAX_DIMENSION)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF maximum image dimensions are %" PRIu32
                      " x %" PRIu32 " per strip/tile.",
                      AVIF_MAX_DIMENSION, AVIF_MAX_DIMENSION);
        return 0;
    }

    /* Bounded by the dimension check above, but verify the multiplication
     * cannot overflow tmsize_t before allocating. */
    {
        tmsize_t rowbytes = TAVIFRasterRowBytes(tif, segment_width);
        if (segment_height != 0 &&
            rowbytes > TIFF_TMSIZE_T_MAX / (tmsize_t)segment_height)
        {
            TIFFErrorExtR(tif, module,
                          "AVIF strip/tile is too large to represent");
            return 0;
        }
        strile_size = rowbytes * (tmsize_t)segment_height;
    }

    if (strile_size == 0)
    {
        TIFFErrorExtR(tif, module, "Empty AVIF strip/tile");
        return 0;
    }

    /* Reuse the accumulation buffer across striles; only grow it. */
    if (strile_size > sp->buffer_size)
    {
        if (sp->pBuffer != NULL)
        {
            _TIFFfreeExt(tif, sp->pBuffer);
            sp->pBuffer = NULL;
        }
        sp->pBuffer = (uint8_t *)_TIFFmallocExt(tif, strile_size);
        if (sp->pBuffer == NULL)
        {
            TIFFErrorExtR(tif, module, "Cannot allocate strile buffer");
            sp->buffer_size = 0;
            return 0;
        }
        sp->buffer_size = strile_size;
    }
    sp->buffer_offset = 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                           */
/* ------------------------------------------------------------------ */

static int TAVIFSetupDecode(TIFF *tif)
{
    static const char module[] = "TAVIFSetupDecode";
    AVIFState *sp = DecoderState(tif);
    uint16_t nBitsPerSample = tif->tif_dir.td_bitspersample;
    uint16_t sampleFormat = tif->tif_dir.td_sampleformat;

    assert(sp != NULL);

    sp->nSamples = tif->tif_dir.td_samplesperpixel;

    if (sp->nSamples != 3 && sp->nSamples != 4)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF driver doesn't support %d bands. Must be 3 (RGB) "
                      "or 4 (RGBA) bands.",
                      sp->nSamples);
        return 0;
    }

    if (!TAVIFSupportedDepth(nBitsPerSample) ||
        sampleFormat != SAMPLEFORMAT_UINT)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF driver requires unsigned integer data with 8, "
                      "10 or 12 bits per sample (got %u bits, sample format "
                      "%u)",
                      (unsigned)nBitsPerSample, (unsigned)sampleFormat);
        return 0;
    }
    sp->bitsPerSample = nBitsPerSample;

    if (sp->state & LSTATE_INIT_ENCODE)
    {
        if (sp->pBuffer != NULL)
        {
            _TIFFfreeExt(tif, sp->pBuffer);
            sp->pBuffer = NULL;
            sp->buffer_size = 0;
        }
        if (sp->pPacked16 != NULL)
        {
            _TIFFfreeExt(tif, sp->pPacked16);
            sp->pPacked16 = NULL;
            sp->packed16_size = 0;
        }
        sp->buffer_offset = 0;
        sp->state = 0;
    }
    sp->state |= LSTATE_INIT_DECODE;
    return 1;
}

static int TAVIFPreDecode(TIFF *tif, uint16_t s)
{
    static const char module[] = "TAVIFPreDecode";
    AVIFState *sp = DecoderState(tif);
    uint32_t segment_width, segment_height;

    (void)s;
    assert(sp != NULL);

    if ((sp->state & LSTATE_INIT_DECODE) == 0)
        tif->tif_setupdecode(tif);

    TAVIFGetStrileDimensions(tif, &segment_width, &segment_height);

    if (segment_width > AVIF_MAX_DIMENSION ||
        segment_height > AVIF_MAX_DIMENSION)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF maximum image dimensions are %" PRIu32
                      " x %" PRIu32 " per strip/tile.",
                      AVIF_MAX_DIMENSION, AVIF_MAX_DIMENSION);
        return 0;
    }

    sp->read_error = 0;
    sp->last_y = 0;
    sp->decode_height = (int)segment_height;
    return 1;
}

/*
 * Decode the whole strile into output (which is either the caller's buffer,
 * when the full strile is requested, or the codec-owned scratch buffer for
 * fractional scanline reads).
 */
static int TAVIFDecodeStrile(TIFF *tif, uint8_t *output,
                             uint32_t segment_width, uint32_t segment_height)
{
    static const char module[] = "TAVIFDecodeStrile";
    AVIFState *sp = DecoderState(tif);
    avifDecoder *decoder = NULL;
    avifImage *image = NULL;
    avifRGBImage rgb;
    avifResult res;
    int rc = 0;

    if (tif->tif_rawcc <= 0 || tif->tif_rawcp == NULL)
    {
        TIFFErrorExtR(tif, module, "Empty AVIF bitstream");
        return 0;
    }

    decoder = avifDecoderCreate();
    if (decoder == NULL)
    {
        TIFFErrorExtR(tif, module, "Cannot allocate AVIF decoder");
        return 0;
    }
    /* Enforce our own dimension ceiling in addition to libavif's limits. */
    decoder->imageDimensionLimit = AVIF_MAX_DIMENSION;

    image = avifImageCreateEmpty();
    if (image == NULL)
    {
        TIFFErrorExtR(tif, module, "Cannot allocate AVIF image");
        goto cleanup;
    }

    res = avifDecoderReadMemory(decoder, image, tif->tif_rawcp,
                                (size_t)tif->tif_rawcc);
    if (res != AVIF_RESULT_OK)
    {
        TAVIFReportResult(tif, module, "decoding", res);
        goto cleanup;
    }

    /* Tile/strip geometry must match exactly: a mismatched blob is a
     * corrupt or deliberately hostile file, not a valid edge tile. */
    if (image->width != segment_width || image->height != segment_height)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF tile/strip mismatch: bitstream is %ux%u, "
                      "expected %ux%u",
                      (unsigned)image->width, (unsigned)image->height,
                      (unsigned)segment_width, (unsigned)segment_height);
        goto cleanup;
    }

    if (!TAVIFSupportedDepth((uint16_t)image->depth))
    {
        TIFFErrorExtR(tif, module,
                      "AVIF bitstream uses unsupported bit depth %u",
                      (unsigned)image->depth);
        goto cleanup;
    }

    if ((uint16_t)image->depth != sp->bitsPerSample)
    {
        TIFFErrorExtR(tif, module,
                      "AVIF bitstream bit depth %u does not match TIFF "
                      "BitsPerSample %u",
                      (unsigned)image->depth,
                      (unsigned)sp->bitsPerSample);
        goto cleanup;
    }

    /*
     * Band handling:
     *  - RGBA raster (4 samples): request RGBA; libavif fills alpha with
     *    255/65535 if the bitstream has no alpha plane.
     *  - RGB raster (3 samples): request RGB; an unexpected alpha plane is
     *    dropped, which is the same tolerance the WebP codec offers.
     * For 10/12-bit depths libavif produces uint16_t samples, which are
     * subsequently packed into the bit-packed libtiff raster layout.
     */
    avifRGBImageSetDefaults(&rgb, image);
    rgb.width = image->width;
    rgb.height = image->height;
    rgb.depth = image->depth;
    rgb.format = (sp->nSamples == 4) ? AVIF_RGB_FORMAT_RGBA
                                     : AVIF_RGB_FORMAT_RGB;

    uint8_t *rgb_pixels = output;
    uint32_t rgb_rowbytes =
        segment_width * sp->nSamples *
        (uint32_t)TAVIFBytesPerSample(sp->bitsPerSample);

    if (sp->bitsPerSample > 8)
    {
        tmsize_t needed16 =
            (tmsize_t)segment_width * (tmsize_t)segment_height *
            (tmsize_t)sp->nSamples * (tmsize_t)sizeof(uint16_t);
        if (needed16 > sp->packed16_size)
        {
            if (sp->pPacked16 != NULL)
                _TIFFfreeExt(tif, sp->pPacked16);
            sp->pPacked16 = (uint16_t *)_TIFFmallocExt(tif, needed16);
            if (sp->pPacked16 == NULL)
            {
                TIFFErrorExtR(tif, module,
                              "Cannot allocate 16-bit conversion buffer");
                sp->packed16_size = 0;
                goto cleanup;
            }
            sp->packed16_size = needed16;
        }
        rgb_pixels = (uint8_t *)sp->pPacked16;
        rgb_rowbytes =
            segment_width * sp->nSamples * (uint32_t)sizeof(uint16_t);
    }

    rgb.pixels = rgb_pixels;
    rgb.rowBytes = rgb_rowbytes;

    res = avifImageYUVToRGB(image, &rgb);
    if (res != AVIF_RESULT_OK)
    {
        TAVIFReportResult(tif, module, "YUV to RGB conversion", res);
        goto cleanup;
    }

    if (sp->bitsPerSample > 8)
    {
        tmsize_t dst_rowbytes =
            TAVIFRasterRowBytes(tif, segment_width);
        uint32_t samples_per_row = segment_width * sp->nSamples;
        /* The destination row starts zeroed so padding bits in the final
         * partial byte are deterministic. */
        for (uint32_t y = 0; y < segment_height; y++)
        {
            uint8_t *dst =
                output + (tmsize_t)y * dst_rowbytes;
            memset(dst, 0, (size_t)dst_rowbytes);
            TAVIFPack16(
                sp->pPacked16 + (uint64_t)y * samples_per_row, dst,
                samples_per_row, sp->bitsPerSample);
        }
    }

    /* ICC profile handling. The authoritative color profile for the TIFF
     * is the ICCProfile tag; cross-check the embedded AVIF profile, when
     * both are present, and flag a mismatch as a distinct error. */
    if (image->icc.size > 0 && image->icc.data != NULL)
    {
        const uint8_t *tif_icc = NULL;
        uint32_t tif_icc_size = 0;
        if (TAVIFGetTIFFICC(tif, &tif_icc, &tif_icc_size))
        {
            if (image->icc.size != tif_icc_size ||
                memcmp(image->icc.data, tif_icc, image->icc.size) != 0)
            {
                TIFFErrorExtR(tif, module,
                              "ICC profile mismatch: AVIF bitstream embeds "
                              "a different profile than the TIFF ICCProfile "
                              "tag");
                goto cleanup;
            }
        }
        /* If only the AVIF bitstream carries a profile, validate its
         * structure so a corrupt profile is reported rather than silently
         * ignored. */
        else if (!TAVIFValidateICC(tif, image->icc.data,
                                   (tmsize_t)image->icc.size))
        {
            goto cleanup;
        }
    }

    rc = 1;

cleanup:
    if (image != NULL)
        avifImageDestroy(image);
    if (decoder != NULL)
        avifDecoderDestroy(decoder);
    return rc;
}

static int TAVIFDecode(TIFF *tif, uint8_t *op, tmsize_t occ, uint16_t s)
{
    static const char module[] = "TAVIFDecode";
    AVIFState *sp = DecoderState(tif);
    uint32_t segment_width, segment_height;
    tmsize_t strile_size;
    tmsize_t rowbytes;
    int decode_whole_strile;

    (void)s;
    assert(sp != NULL);
    assert(sp->state == LSTATE_INIT_DECODE);

    if (sp->read_error)
    {
        _TIFFmemset(op, 0, (size_t)occ);
        TIFFErrorExtR(tif, module,
                      "Scanline %" PRIu32
                      " cannot be read due to a previous error",
                      tif->tif_dir.td_row);
        return 0;
    }

    TAVIFGetStrileDimensions(tif, &segment_width, &segment_height);

    rowbytes = TAVIFRasterRowBytes(tif, segment_width);
    strile_size = rowbytes * (tmsize_t)segment_height;

    /* Fractional scanline reads must be row-aligned. */
    if (occ % rowbytes != 0)
    {
        _TIFFmemset(op, 0, (size_t)occ);
        TIFFErrorExtR(tif, module, "Fractional scanlines cannot be read");
        return 0;
    }

    decode_whole_strile = (occ == strile_size);

    if (!decode_whole_strile)
    {
        /* Decode once into the scratch buffer and serve rows from it. */
        if (sp->last_y == 0)
        {
            if (strile_size > sp->decode_buffer_size)
            {
                if (sp->pDecodeBuffer != NULL)
                {
                    _TIFFfreeExt(tif, sp->pDecodeBuffer);
                    sp->pDecodeBuffer = NULL;
                }
                sp->pDecodeBuffer =
                    (uint8_t *)_TIFFmallocExt(tif, strile_size);
                if (sp->pDecodeBuffer == NULL)
                {
                    TIFFErrorExtR(tif, module,
                                  "Cannot allocate decode buffer");
                    sp->decode_buffer_size = 0;
                    sp->read_error = 1;
                    return 0;
                }
                sp->decode_buffer_size = strile_size;
            }

            if (!TAVIFDecodeStrile(tif, sp->pDecodeBuffer,
                                   segment_width, segment_height))
            {
                _TIFFmemset(op, 0, (size_t)occ);
                sp->read_error = 1;
                return 0;
            }
        }

        {
            int rows_requested = (int)(occ / rowbytes);
            if (sp->last_y + rows_requested > sp->decode_height)
            {
                _TIFFmemset(op, 0, (size_t)occ);
                sp->read_error = 1;
                TIFFErrorExtR(tif, module,
                              "AVIF decode: more rows requested than the "
                              "strip/tile contains");
                return 0;
            }
            _TIFFmemcpy(op, sp->pDecodeBuffer +
                                (tmsize_t)sp->last_y * rowbytes,
                        (size_t)occ);
            sp->last_y += rows_requested;
        }
    }
    else
    {
        if (!TAVIFDecodeStrile(tif, op, segment_width, segment_height))
        {
            _TIFFmemset(op, 0, (size_t)occ);
            sp->read_error = 1;
            return 0;
        }
        /* Only a whole-strile decode consumes the raw bitstream. For
         * fractional (scanline) reads the same raw strile must remain
         * available for the remaining rows, which are served from the
         * scratch buffer. */
        tif->tif_rawcp += tif->tif_rawcc;
        tif->tif_rawcc = 0;
    }

    return 1;
}

static int TAVIFFixupTags(TIFF *tif)
{
    static const char module[] = "TAVIFFixupTags";

    if (tif->tif_dir.td_planarconfig != PLANARCONFIG_CONTIG)
    {
        TIFFErrorExtR(tif, module,
                      "TIFF AVIF requires contiguous planar configuration "
                      "(RGBRGB... or RGBARGBA...)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pseudo tags                                                        */
/* ------------------------------------------------------------------ */

static int TAVIFVSetField(TIFF *tif, uint32_t tag, va_list ap)
{
    static const char module[] = "TAVIFVSetField";
    AVIFState *sp = LState(tif);

    switch (tag)
    {
        case TIFFTAG_AVIF_QUALITY:
        {
            int val = va_arg(ap, int);
            if (val < 0 || val > 100)
            {
                TIFFErrorExtR(tif, module,
                              "AVIF quality must be between 0 and 100");
                return 0;
            }
            sp->quality = val;
            return 1;
        }
        case TIFFTAG_AVIF_LOSSLESS:
            sp->lossless = va_arg(ap, int) ? 1 : 0;
            return 1;
        case TIFFTAG_AVIF_SPEED:
        {
            int val = va_arg(ap, int);
            if (val < 0 || val > 10)
            {
                TIFFErrorExtR(tif, module,
                              "AVIF speed must be between 0 and 10");
                return 0;
            }
            sp->speed = val;
            return 1;
        }
        case TIFFTAG_AVIF_SUBSAMPLING:
        {
            int val = va_arg(ap, int);
            if (val != AVIF_SUBSAMPLING_444 &&
                val != AVIF_SUBSAMPLING_422 &&
                val != AVIF_SUBSAMPLING_420)
            {
                TIFFErrorExtR(tif, module,
                              "AVIF subsampling must be 0 (4:4:4), "
                              "1 (4:2:2) or 2 (4:2:0)");
                return 0;
            }
            sp->subsampling = val;
            return 1;
        }
        default:
            return (*sp->vsetparent)(tif, tag, ap);
    }
    /*NOTREACHED*/
}

static int TAVIFVGetField(TIFF *tif, uint32_t tag, va_list ap)
{
    AVIFState *sp = LState(tif);

    switch (tag)
    {
        case TIFFTAG_AVIF_QUALITY:
            *va_arg(ap, int *) = sp->quality;
            break;
        case TIFFTAG_AVIF_LOSSLESS:
            *va_arg(ap, int *) = sp->lossless;
            break;
        case TIFFTAG_AVIF_SPEED:
            *va_arg(ap, int *) = sp->speed;
            break;
        case TIFFTAG_AVIF_SUBSAMPLING:
            *va_arg(ap, int *) = sp->subsampling;
            break;
        default:
            return (*sp->vgetparent)(tif, tag, ap);
    }
    return 1;
}

static const TIFFField TAVIFFields[] = {
    {TIFFTAG_AVIF_QUALITY, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT, FIELD_PSEUDO,
     TRUE, FALSE, "AVIF quality", NULL},
    {TIFFTAG_AVIF_LOSSLESS, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT, FIELD_PSEUDO,
     TRUE, FALSE, "AVIF lossless/lossy", NULL},
    {TIFFTAG_AVIF_SPEED, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT, FIELD_PSEUDO,
     TRUE, FALSE, "AVIF encoder speed", NULL},
    {TIFFTAG_AVIF_SUBSAMPLING, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT,
     FIELD_PSEUDO, TRUE, FALSE, "AVIF chroma subsampling", NULL},
};

/*
 * The read path uses this ratio as a sanity guard against absurd strip
 * byte counts. AVIF is a modern lossy/lossless codec similar in spirit to
 * WebP; use the same conservative (large) estimate as the WebP lossless
 * case so valid files are never rejected by the heuristic.
 */
static uint64_t TAVIFGetMaxCompressionRatio(TIFF *tif)
{
    return (tif->tif_dir.td_samplesperpixel == 4) ? 104194 : 78146;
}

static void TAVIFCleanup(TIFF *tif)
{
    AVIFState *sp = LState(tif);

    assert(sp != 0);

    tif->tif_tagmethods.vgetfield = sp->vgetparent;
    tif->tif_tagmethods.vsetfield = sp->vsetparent;

    if (sp->pBuffer != NULL)
    {
        _TIFFfreeExt(tif, sp->pBuffer);
        sp->pBuffer = NULL;
    }
    if (sp->pPacked16 != NULL)
    {
        _TIFFfreeExt(tif, sp->pPacked16);
        sp->pPacked16 = NULL;
    }
    if (sp->pDecodeBuffer != NULL)
    {
        _TIFFfreeExt(tif, sp->pDecodeBuffer);
        sp->pDecodeBuffer = NULL;
    }

    _TIFFfreeExt(tif, tif->tif_data);
    tif->tif_data = NULL;

    _TIFFSetDefaultCompressionState(tif);
}

int TIFFInitAVIF(TIFF *tif, int scheme)
{
    static const char module[] = "TIFFInitAVIF";
    AVIFState *sp;

    (void)scheme;
    assert(scheme == COMPRESSION_AVIF);

    if (!_TIFFMergeFields(tif, TAVIFFields, TIFFArrayCount(TAVIFFields)))
    {
        TIFFErrorExtR(tif, module,
                      "Merging AVIF codec-specific tags failed");
        return 0;
    }

    tif->tif_data = (uint8_t *)_TIFFmallocExt(tif, sizeof(AVIFState));
    if (tif->tif_data == NULL)
        goto bad;
    sp = LState(tif);
    _TIFFmemset(sp, 0, sizeof(AVIFState));

    sp->vgetparent = tif->tif_tagmethods.vgetfield;
    tif->tif_tagmethods.vgetfield = TAVIFVGetField;
    sp->vsetparent = tif->tif_tagmethods.vsetfield;
    tif->tif_tagmethods.vsetfield = TAVIFVSetField;

    /* Defaults. */
    sp->quality = 60;             /* libavif's default quality */
    sp->lossless = 0;
    sp->speed = AVIF_SPEED_DEFAULT;
    sp->subsampling = AVIF_SUBSAMPLING_444;
    sp->state = 0;
    sp->nSamples = 0;
    sp->bitsPerSample = 0;
    sp->read_error = 0;
    sp->pBuffer = NULL;
    sp->buffer_size = 0;
    sp->buffer_offset = 0;
    sp->pPacked16 = NULL;
    sp->packed16_size = 0;
    sp->pDecodeBuffer = NULL;
    sp->decode_buffer_size = 0;
    sp->last_y = 0;

    tif->tif_fixuptags = TAVIFFixupTags;
    tif->tif_setupdecode = TAVIFSetupDecode;
    tif->tif_predecode = TAVIFPreDecode;
    tif->tif_decoderow = TAVIFDecode;
    tif->tif_decodestrip = TAVIFDecode;
    tif->tif_decodetile = TAVIFDecode;
    tif->tif_setupencode = TAVIFSetupEncode;
    tif->tif_preencode = TAVIFPreEncode;
    tif->tif_postencode = TAVIFPostEncode;
    tif->tif_encoderow = TAVIFEncode;
    tif->tif_encodestrip = TAVIFEncode;
    tif->tif_encodetile = TAVIFEncode;
    tif->tif_cleanup = TAVIFCleanup;
    tif->tif_getmaxcompressionratio = TAVIFGetMaxCompressionRatio;

    return 1;
bad:
    TIFFErrorExtR(tif, module, "No space for AVIF state block");
    return 0;
}

#endif /* AVIF_SUPPORT */

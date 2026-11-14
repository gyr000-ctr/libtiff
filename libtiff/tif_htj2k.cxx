/*
 * Copyright (c) 2026, LibTIFF contributors
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
 * IN NO EVENT SHALL SAM LEFFER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef HTJ2K_SUPPORT

/*
 * TIFF Library.
 *
 * High Throughput JPEG 2000 (HTJ2K / ISO/IEC 15444-15) compression,
 * implemented on top of the OpenJPH library (https://github.com/aous72/OpenJPH).
 *
 * Design notes:
 *  - One independent HTJ2K codestream is stored per TIFF strip or tile
 *    (and per component when PlanarConfiguration is SEPARATE).
 *  - Encoding and decoding stream tile/strip data through OpenJPH one line
 *    at a time.  Apart from the strip/tile sample accumulation buffer that
 *    every libtiff codec needs and the compressed codestream buffer handed
 *    to the raw layer, no whole-image copy is made.
 *  - OpenJPH reports fatal errors by throwing C++ exceptions; a process-wide
 *    error handler turns them into libtiff diagnostics.
 */

#include <openjph/ojph_arch.h>
#include <openjph/ojph_codestream.h>
#include <openjph/ojph_file.h>
#include <openjph/ojph_mem.h>
#include <openjph/ojph_message.h>
#include <openjph/ojph_params.h>

#include <exception>
#include <stdexcept>

#define HTJ2KSTATE_INIT_DECODE 0x01
#define HTJ2KSTATE_INIT_ENCODE 0x02

/* OpenJPH works on at most 16-bit samples internally (32-bit si32 lines). */
#define HTJ2K_MAX_BITDEPTH 16

typedef struct
{
    int state; /* HTJ2KSTATE_* flags */

    int reversible;     /* lossless vs lossy (near-lossless) */
    int num_decomps;    /* wavelet decomposition levels */
    double qstep;       /* quantization step for lossy mode */

    uint32_t seg_width;   /* actual width of the current strip/tile */
    uint32_t seg_height;  /* actual height of the current strip/tile */
    uint32_t nom_width;   /* nominal (padded) tile/strip width */
    uint32_t nom_height;  /* nominal (padded) tile/strip height */
    uint32_t ncomponents; /* number of HTJ2K components in this codestream */
    uint32_t comp_index;  /* TIFF sample index of this codestream */
    uint32_t ds_x;        /* horizontal subsampling of this component */
    uint32_t ds_y;        /* vertical subsampling of this component */
    uint32_t cs_width;    /* codestream width  for this component */
    uint32_t cs_height;   /* codestream height for this component */
    int is_signed;
    uint16_t sample_bytes;

    tmsize_t accum_size; /* valid bytes for an actual (partial) strile */
    uint8_t *accum;      /* strip/tile sample accumulation buffer */
    uint8_t *sub_buffer; /* compact buffer for a subsampled component */
    tmsize_t sub_buffer_capacity;
    tmsize_t accum_used; /* write-side fill level */
    tmsize_t accum_read; /* read-side consumption offset */
    tmsize_t accum_capacity; /* allocation size (== padded strile size) */
    /* Tiles are always presented by libtiff with padded nominal geometry,
     * while the final strip is tightly packed to its real dimensions. */
    int padded_layout;

    TIFFVGetMethod vgetparent; /* super-class methods */
    TIFFVSetMethod vsetparent;
} HTJ2KState;

#define GetHTJ2KState(tif) ((HTJ2KState *)(tif)->tif_data)

/* ------------------------------------------------------------------ */
/* OpenJPH error/warning interception.                                 */
/* OpenJPH's messaging configuration is process-global, so a single    */
/* handler is installed once and merely routes the text through the    */
/* diagnostic machinery of the TIFF handle that is currently active.   */
/* ------------------------------------------------------------------ */

static TIFF *htj2k_active_tif = NULL;

class HTJ2KErrorHandler : public ojph::message_error
{
  public:
    void operator()(int code, const char *file_name, int line_num,
                    const char *fmt, ...) override
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (htj2k_active_tif != NULL)
        {
            TIFFErrorExtR(htj2k_active_tif, "OpenJPH",
                          "HTJ2K codestream error: %s", buf);
        }
        throw std::runtime_error(buf);
    }
};

class HTJ2KWarningHandler : public ojph::message_warning
{
  public:
    void operator()(int code, const char *file_name, int line_num,
                    const char *fmt, ...) override
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (htj2k_active_tif != NULL)
        {
            TIFFWarningExtR(htj2k_active_tif, "OpenJPH",
                            "HTJ2K codestream warning: %s", buf);
        }
    }
};

static HTJ2KErrorHandler htj2k_error_handler;
static HTJ2KWarningHandler htj2k_warning_handler;
static bool htj2k_handlers_installed = false;

static void HTJ2KInstallHandlers(void)
{
    if (!htj2k_handlers_installed)
    {
        ojph::configure_error(&htj2k_error_handler);
        ojph::configure_warning(&htj2k_warning_handler);
        ojph::set_info_stream(NULL);
        htj2k_handlers_installed = true;
    }
}

/* RAII guard so the active handle is always restored, even on throw. */
struct HTJ2KActiveGuard
{
    TIFF *saved;
    explicit HTJ2KActiveGuard(TIFF *tif) : saved(htj2k_active_tif)
    {
        htj2k_active_tif = tif;
    }
    ~HTJ2KActiveGuard() { htj2k_active_tif = saved; }
};

/* ------------------------------------------------------------------ */
/* Geometry / parameter validation                                     */
/* ------------------------------------------------------------------ */

static int HTJ2KCheckConfiguration(TIFF *tif, const char *module)
{
    TIFFDirectory *td = &tif->tif_dir;

    if (td->td_bitspersample > HTJ2K_MAX_BITDEPTH ||
        td->td_bitspersample == 0)
    {
        TIFFErrorExtR(tif, module,
                      "Unsupported BitsPerSample=%u for HTJ2K "
                      "(only 1..%u bit samples are supported)",
                      (unsigned)td->td_bitspersample,
                      HTJ2K_MAX_BITDEPTH);
        return 0;
    }
    if (td->td_sampleformat == SAMPLEFORMAT_IEEEFP ||
        td->td_sampleformat == SAMPLEFORMAT_COMPLEXIEEEFP ||
        td->td_sampleformat == SAMPLEFORMAT_COMPLEXINT)
    {
        TIFFErrorExtR(tif, module,
                      "HTJ2K only supports integer samples "
                      "(SampleFormat UINT/INT), got %u",
                      (unsigned)td->td_sampleformat);
        return 0;
    }
    if (td->td_sampleformat != SAMPLEFORMAT_UINT &&
        td->td_sampleformat != SAMPLEFORMAT_INT)
    {
        TIFFErrorExtR(tif, module, "Unsupported SampleFormat=%u for HTJ2K",
                      (unsigned)td->td_sampleformat);
        return 0;
    }
    if (td->td_samplesperpixel == 0)
    {
        TIFFErrorExtR(tif, module, "Invalid SamplesPerPixel=0");
        return 0;
    }
    /* Padded sub-byte samples (e.g. 4-bit) cannot be addressed as int. */
    if (td->td_bitspersample < 8)
    {
        TIFFErrorExtR(tif, module,
                      "HTJ2K requires BitsPerSample >= 8, got %u",
                      (unsigned)td->td_bitspersample);
        return 0;
    }
    if (td->td_planarconfig != PLANARCONFIG_CONTIG &&
        td->td_planarconfig != PLANARCONFIG_SEPARATE)
    {
        TIFFErrorExtR(tif, module, "Invalid PlanarConfiguration=%u",
                      (unsigned)td->td_planarconfig);
        return 0;
    }
    if (td->td_planarconfig == PLANARCONFIG_CONTIG &&
        (td->td_photometric == PHOTOMETRIC_YCBCR) &&
        (td->td_ycbcrsubsampling[0] != 1 ||
         td->td_ycbcrsubsampling[1] != 1))
    {
        TIFFErrorExtR(tif, module,
                      "Chroma subsampling (%u,%u) is only supported with "
                      "PlanarConfiguration=SEPARATE",
                      (unsigned)td->td_ycbcrsubsampling[0],
                      (unsigned)td->td_ycbcrsubsampling[1]);
        return 0;
    }
    return 1;
}

/* Number of HTJ2K components carried by a single strip/tile codestream. */
static uint32_t HTJ2KComponentsPerStrile(TIFF *tif)
{
    TIFFDirectory *td = &tif->tif_dir;
    return (td->td_planarconfig == PLANARCONFIG_CONTIG)
               ? td->td_samplesperpixel
               : 1;
}

/* Subsampling factors for a given TIFF sample/component index.  Only the
 * SEPARATE planar layout supports non-unit factors: the luma component is
 * full resolution and every other component uses the YCbCr subsampling
 * factors declared in the directory. */
static void HTJ2KComponentSubsampling(TIFF *tif, uint32_t comp_index,
                                      uint32_t *ds_x, uint32_t *ds_y)
{
    TIFFDirectory *td = &tif->tif_dir;
    if (td->td_planarconfig == PLANARCONFIG_SEPARATE && comp_index > 0 &&
        td->td_photometric == PHOTOMETRIC_YCBCR)
    {
        *ds_x = td->td_ycbcrsubsampling[0] ? td->td_ycbcrsubsampling[0] : 1;
        *ds_y = td->td_ycbcrsubsampling[1] ? td->td_ycbcrsubsampling[1] : 1;
    }
    else
    {
        *ds_x = 1;
        *ds_y = 1;
    }
}

static int HTJ2KSegmentGeometry(TIFF *tif, uint32_t *width, uint32_t *height,
                                uint32_t *ncomp)
{
    static const char module[] = "HTJ2KSegmentGeometry";
    TIFFDirectory *td = &tif->tif_dir;

    if (isTiled(tif))
    {
        if (td->td_tilewidth == 0 || td->td_tilelength == 0)
        {
            TIFFErrorExtR(tif, module, "Invalid tile dimensions %ux%u",
                          (unsigned)td->td_tilewidth,
                          (unsigned)td->td_tilelength);
            return 0;
        }
        *width = td->td_tilewidth;
        *height = td->td_tilelength;
    }
    else
    {
        if (td->td_rowsperstrip == 0)
        {
            TIFFErrorExtR(tif, module, "Invalid RowsPerStrip=0");
            return 0;
        }
        *width = td->td_imagewidth;
        *height = td->td_rowsperstrip;
    }
    *ncomp = HTJ2KComponentsPerStrile(tif);

    /* Overflow guard: reject configurations whose strile cannot be indexed
     * with tmsize_t. */
    uint64_t pixels = (uint64_t)(*width) * (uint64_t)(*height);
    if (pixels > (uint64_t)TIFF_TMSIZE_T_MAX /
                      ((uint64_t)(*ncomp) * (uint64_t)td->td_bitspersample /
                       8U))
    {
        TIFFErrorExtR(tif, module,
                      "Strip/tile of %ux%u with %u component(s) is too large",
                      (unsigned)*width, (unsigned)*height, (unsigned)*ncomp);
        return 0;
    }
    return 1;
}

/* Actual, possibly partial, geometry of the strip/tile about to be
 * processed.  Tiles on the right/bottom image edge and the final strip can
 * be smaller than the nominal tile/strip size. */
static int HTJ2KActualGeometry(TIFF *tif, uint32_t nominal_w,
                               uint32_t nominal_h, uint32_t ncomp,
                               uint32_t *actual_w, uint32_t *actual_h)
{
    static const char module[] = "HTJ2KActualGeometry";
    TIFFDirectory *td = &tif->tif_dir;

    if (isTiled(tif))
    {
        uint32_t tile = td->td_curtile;
        uint32_t tiles_x = TIFFhowmany_32(td->td_imagewidth,
                                          td->td_tilewidth);
        uint32_t tiles_per_image =
            tiles_x * TIFFhowmany_32(td->td_imagelength,
                                     td->td_tilelength);
        if (tile >= tiles_per_image *
                         (td->td_planarconfig == PLANARCONFIG_SEPARATE
                              ? td->td_samplesperpixel
                              : 1))
        {
            TIFFErrorExtR(tif, module, "Tile %u is out of range (have %u)",
                          (unsigned)tile,
                          (unsigned)(tiles_per_image * td->td_samplesperpixel));
            return 0;
        }
        uint32_t tile_in_image =
            tile % (tiles_per_image ? tiles_per_image : 1);
        uint32_t tx = tile_in_image % (tiles_x ? tiles_x : 1);
        uint32_t ty = tile_in_image / (tiles_x ? tiles_x : 1);
        uint32_t x0 = tx * td->td_tilewidth;
        uint32_t y0 = ty * td->td_tilelength;
        *actual_w = (x0 + nominal_w > td->td_imagewidth)
                        ? td->td_imagewidth - x0
                        : nominal_w;
        *actual_h = (y0 + nominal_h > td->td_imagelength)
                        ? td->td_imagelength - y0
                        : nominal_h;
    }
    else
    {
        uint32_t strip = td->td_curstrip;
        uint32_t strips_per_image = td->td_stripsperimage
                                        ? td->td_stripsperimage
                                        : 1;
        if (strip >= strips_per_image *
                          (td->td_planarconfig == PLANARCONFIG_SEPARATE
                               ? td->td_samplesperpixel
                               : 1))
        {
            TIFFErrorExtR(tif, module,
                          "Strip %u is out of range (have %u strips)",
                          (unsigned)strip,
                          (unsigned)(strips_per_image *
                                     td->td_samplesperpixel));
            return 0;
        }
        uint32_t strip_in_image = strip % strips_per_image;
        uint32_t row0 = strip_in_image * td->td_rowsperstrip;
        *actual_w = nominal_w;
        *actual_h = (row0 + nominal_h > td->td_imagelength)
                        ? td->td_imagelength - row0
                        : nominal_h;
    }

    if (*actual_w == 0 || *actual_h == 0 || ncomp == 0)
    {
        TIFFErrorExtR(tif, module,
                      "Invalid actual strip/tile geometry %ux%u with %u "
                      "component(s)",
                      (unsigned)*actual_w, (unsigned)*actual_h,
                      (unsigned)ncomp);
        return 0;
    }
    return 1;
}

static int HTJ2KSetupBuffers(TIFF *tif)
{
    static const char module[] = "HTJ2KSetupBuffers";
    HTJ2KState *sp = GetHTJ2KState(tif);
    TIFFDirectory *td = &tif->tif_dir;
    uint32_t width, height, ncomp;
    uint32_t actual_w, actual_h;

    if (!HTJ2KSegmentGeometry(tif, &width, &height, &ncomp))
        return 0;
    if (!HTJ2KActualGeometry(tif, width, height, ncomp, &actual_w,
                             &actual_h))
        return 0;

    sp->seg_width = actual_w;
    sp->seg_height = actual_h;
    sp->nom_width = width;
    sp->nom_height = height;
    sp->ncomponents = ncomp;
    sp->padded_layout = isTiled(tif) ? 1 : 0;
    sp->is_signed = (td->td_sampleformat == SAMPLEFORMAT_INT);
    sp->sample_bytes = (uint16_t)(td->td_bitspersample / 8);

    /* For SEPARATE planar data each strile holds one component; derive its
     * sample index and any per-component subsampling. */
    if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
    {
        if (isTiled(tif))
        {
            uint32_t tiles_per_image =
                TIFFhowmany_32(td->td_imagewidth, td->td_tilewidth) *
                TIFFhowmany_32(td->td_imagelength, td->td_tilelength);
            sp->comp_index = tiles_per_image
                                 ? td->td_curtile / tiles_per_image
                                 : 0;
        }
        else
        {
            sp->comp_index = td->td_stripsperimage
                                 ? td->td_curstrip / td->td_stripsperimage
                                 : 0;
        }
    }
    else
    {
        sp->comp_index = 0;
    }
    HTJ2KComponentSubsampling(tif, sp->comp_index, &sp->ds_x, &sp->ds_y);

    /* Codestream dimensions after subsampling (rounded up). */
    sp->cs_width = TIFFhowmany_32(actual_w, sp->ds_x);
    sp->cs_height = TIFFhowmany_32(actual_h, sp->ds_y);
    if (sp->cs_width == 0 || sp->cs_height == 0)
    {
        TIFFErrorExtR(tif, module,
                      "Subsampled component %u has zero dimensions",
                      (unsigned)sp->comp_index);
        return 0;
    }

    /* The accumulation buffer always holds the full-resolution component
     * plane as presented by the TIFF layer; subsampling only affects the
     * geometry declared to OpenJPH, not this buffer's accounting.
     * Tiles are padded to the nominal size, the final strip is tightly
     * packed to its real dimensions. */
    uint64_t cap64 = (uint64_t)width * (uint64_t)height *
                     (uint64_t)ncomp * (uint64_t)sp->sample_bytes;
    uint64_t size64 = (uint64_t)actual_w * (uint64_t)actual_h *
                      (uint64_t)ncomp * (uint64_t)sp->sample_bytes;
    if (cap64 > (uint64_t)TIFF_TMSIZE_T_MAX ||
        size64 > (uint64_t)TIFF_TMSIZE_T_MAX)
    {
        TIFFErrorExtR(tif, module, "Strip/tile buffer is too large");
        return 0;
    }
    sp->accum_size = (tmsize_t)size64;
    sp->accum_used = 0;

    if (sp->accum_capacity < (tmsize_t)cap64)
    {
        if (sp->accum)
            _TIFFfreeExt(tif, sp->accum);
        sp->accum = (uint8_t *)_TIFFmallocExt(tif, (tmsize_t)cap64);
        if (sp->accum == NULL)
        {
            TIFFErrorExtR(tif, module,
                          "Cannot allocate %" PRIu64 " bytes for HTJ2K buffer",
                          cap64);
            sp->accum_capacity = 0;
            return 0;
        }
        sp->accum_capacity = (tmsize_t)cap64;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sample conversion (OpenJPH si32 lines -> TIFF native layout)        */
/* ------------------------------------------------------------------ */

static void HTJ2KStoreLine(const ojph::si32 *src, uint8_t *dst,
                           uint32_t width, uint16_t sample_bytes,
                           int is_signed)
{
    if (sample_bytes == 1)
    {
        if (is_signed)
        {
            int8_t *p = (int8_t *)dst;
            for (uint32_t x = 0; x < width; ++x)
                p[x] = (int8_t)src[x];
        }
        else
        {
            for (uint32_t x = 0; x < width; ++x)
                dst[x] = (uint8_t)src[x];
        }
    }
    else
    {
        if (is_signed)
        {
            int16_t *p = (int16_t *)dst;
            for (uint32_t x = 0; x < width; ++x)
                p[x] = (int16_t)src[x];
        }
        else
        {
            uint16_t *p = (uint16_t *)dst;
            for (uint32_t x = 0; x < width; ++x)
                p[x] = (uint16_t)src[x];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Encoding                                                            */
/* ------------------------------------------------------------------ */

static int HTJ2KSetupEncode(TIFF *tif)
{
    HTJ2KState *sp = GetHTJ2KState(tif);
    if (sp->state & HTJ2KSTATE_INIT_DECODE)
        sp->state = 0;
    sp->state |= HTJ2KSTATE_INIT_ENCODE;
    return 1;
}

static int HTJ2KPreEncode(TIFF *tif, uint16_t s)
{
    static const char module[] = "HTJ2KPreEncode";
    HTJ2KState *sp = GetHTJ2KState(tif);
    (void)s;
    if (sp->state != HTJ2KSTATE_INIT_ENCODE)
        tif->tif_setupencode(tif);
    if (!HTJ2KCheckConfiguration(tif, module))
        return 0;
    if (!HTJ2KSetupBuffers(tif))
        return 0;
    sp->accum_used = 0;
    return 1;
}

static int HTJ2KEncode(TIFF *tif, uint8_t *bp, tmsize_t cc, uint16_t s)
{
    static const char module[] = "HTJ2KEncode";
    HTJ2KState *sp = GetHTJ2KState(tif);
    (void)s;
    if ((uint64_t)sp->accum_used + (uint64_t)cc >
        (uint64_t)sp->accum_capacity)
    {
        TIFFErrorExtR(tif, module,
                      "Too many bytes written to HTJ2K strip/tile "
                      "(%" PRIu64 " > %" PRIu64 ")",
                      (uint64_t)sp->accum_used + (uint64_t)cc,
                      (uint64_t)sp->accum_capacity);
        return 0;
    }
    _TIFFmemcpy(sp->accum + sp->accum_used, bp, (size_t)cc);
    sp->accum_used += cc;
    return 1;
}

static int HTJ2KPostEncode(TIFF *tif)
{
    static const char module[] = "HTJ2KPostEncode";
    HTJ2KState *sp = GetHTJ2KState(tif);
    TIFFDirectory *td = &tif->tif_dir;

    /* Tiles are always padded to the nominal tile size.  For strips the
     * final strile is tightly packed to its real dimensions.  This is
     * independent of subsampling, which only changes the HTJ2K codestream
     * geometry, not the TIFF sample plane. */
    const tmsize_t expected_used =
        sp->padded_layout ? sp->accum_capacity : sp->accum_size;
    if (sp->accum_used != expected_used)
    {
        TIFFErrorExtR(tif, module,
                      "Unexpected number of bytes in HTJ2K buffer "
                      "(%" PRIu64 " != %" PRIu64 ")",
                      (uint64_t)sp->accum_used, (uint64_t)expected_used);
        return 0;
    }

    HTJ2KInstallHandlers();
    HTJ2KActiveGuard guard(tif);

    try
    {
        ojph::codestream cs;
        const bool planar = (td->td_planarconfig == PLANARCONFIG_SEPARATE);
        cs.set_planar(planar);

        ojph::param_siz siz = cs.access_siz();
        siz.set_image_extent(ojph::point(sp->seg_width, sp->seg_height));
        siz.set_tile_size(ojph::size(sp->seg_width, sp->seg_height));
        siz.set_image_offset(ojph::point(0, 0));
        siz.set_tile_offset(ojph::point(0, 0));
        siz.set_num_components(sp->ncomponents);
        for (uint32_t c = 0; c < sp->ncomponents; ++c)
        {
            /* A SEPARATE codestream holds a single component, which may be
             * subsampled; CONTIG codestreams always use unit sampling. */
            ojph::point ds((ojph::si32)sp->ds_x, (ojph::si32)sp->ds_y);
            siz.set_component(c, ds, td->td_bitspersample, sp->is_signed);
        }

        ojph::param_cod cod = cs.access_cod();
        cod.set_reversible(sp->reversible != 0);
        cod.set_num_decomposition((ojph::ui32)sp->num_decomps);
        /* The irreversible RCT/ICT color transform only makes sense for
         * three interleaved color components. */
        bool color_transform =
            (sp->ncomponents == 3 &&
             td->td_planarconfig == PLANARCONFIG_CONTIG &&
             (td->td_photometric == PHOTOMETRIC_RGB ||
              td->td_photometric == PHOTOMETRIC_YCBCR));
        cod.set_color_transform(color_transform);
        if (!sp->reversible)
        {
            ojph::param_qcd qcd = cs.access_qcd();
            qcd.set_irrev_quant((float)sp->qstep);
        }

        ojph::mem_outfile outfile;
        outfile.open();
        cs.write_headers(&outfile);

        /* Strides follow the in-buffer layout: nominal (padded) for tiles,
         * actual (tight) for strips. Only seg_* samples per row are valid. */
        const uint32_t stride_w =
            sp->padded_layout ? sp->nom_width : sp->seg_width;
        const uint32_t row_stride =
            stride_w * sp->ncomponents * sp->sample_bytes;
        ojph::ui32 next_comp = 0;
        ojph::line_buf *line = NULL;
        uint32_t lines_done = 0;
        /* OpenJPH always exchanges one component-row at a time, in both
         * planar and interleaved modes.  A subsampled SEPARATE component
         * contributes cs_height rows rather than seg_height. */
        const uint32_t expected_lines =
            planar ? sp->cs_height * sp->ncomponents
                   : sp->seg_height * sp->ncomponents;

        for (;;)
        {
            line = cs.exchange(line, next_comp);
            if (line == NULL)
                break;
            if (lines_done >= expected_lines)
            {
                TIFFErrorExtR(tif, module,
                              "OpenJPH requested more lines than available");
                return 0;
            }
            if (next_comp >= sp->ncomponents)
            {
                TIFFErrorExtR(tif, module,
                              "OpenJPH returned invalid component index %u",
                              (unsigned)next_comp);
                return 0;
            }
            const uint8_t *row;
            if (planar)
            {
                /* Single component; map the codestream row back to the
                 * subsampled position in the TIFF component plane. */
                uint32_t y = lines_done;
                uint32_t src_y = y * sp->ds_y;
                row = sp->accum +
                      (tmsize_t)src_y * stride_w * sp->sample_bytes;
            }
            else
            {
                /* Interleaved ordering: component rows alternate for each
                 * image row (row0c0, row0c1, ..., row1c0, ...). */
                uint32_t y = lines_done / sp->ncomponents;
                row = sp->accum + (tmsize_t)y * row_stride;
            }

            if (sp->sample_bytes == 1)
            {
                ojph::si32 *p = line->i32;
                if (planar)
                {
                    if (sp->is_signed)
                    {
                        const int8_t *r = (const int8_t *)row;
                        for (uint32_t x = 0; x < sp->cs_width; ++x)
                            p[x] = r[x * sp->ds_x];
                    }
                    else
                    {
                        for (uint32_t x = 0; x < sp->cs_width; ++x)
                            p[x] = row[x * sp->ds_x];
                    }
                }
                else
                {
                    /* Deinterleave samples for this component. */
                    if (sp->is_signed)
                    {
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            p[x] = ((const int8_t *)row)[x * sp->ncomponents +
                                                         next_comp];
                    }
                    else
                    {
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            p[x] = row[x * sp->ncomponents + next_comp];
                    }
                }
            }
            else
            {
                ojph::si32 *p = line->i32;
                if (planar)
                {
                    if (sp->is_signed)
                    {
                        const int16_t *r = (const int16_t *)row;
                        for (uint32_t x = 0; x < sp->cs_width; ++x)
                            p[x] = r[x * sp->ds_x];
                    }
                    else
                    {
                        const uint16_t *r = (const uint16_t *)row;
                        for (uint32_t x = 0; x < sp->cs_width; ++x)
                            p[x] = r[x * sp->ds_x];
                    }
                }
                else
                {
                    if (sp->is_signed)
                    {
                        const int16_t *r = (const int16_t *)row;
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            p[x] = r[x * sp->ncomponents + next_comp];
                    }
                    else
                    {
                        const uint16_t *r = (const uint16_t *)row;
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            p[x] = r[x * sp->ncomponents + next_comp];
                    }
                }
            }
            ++lines_done;
        }

        if (lines_done != expected_lines)
        {
            TIFFErrorExtR(tif, module,
                          "OpenJPH consumed %u lines, expected %u",
                          lines_done, expected_lines);
            return 0;
        }

        cs.flush();
        tmsize_t out_size = (tmsize_t)outfile.tell();
        if (out_size <= 0)
        {
            TIFFErrorExtR(tif, module, "Empty HTJ2K codestream produced");
            return 0;
        }
        /* mem_outfile frees its storage on close(), so publish first. */
        uint8_t *raw_backup = tif->tif_rawdata;
        tif->tif_rawdata = (uint8_t *)outfile.get_data();
        tif->tif_rawcc = out_size;
        int ok = TIFFFlushData1(tif);
        tif->tif_rawdata = raw_backup;
        tif->tif_rawcc = 0;
        cs.close();
        if (!ok)
            return 0;
        return 1;
    }
    catch (const std::exception &e)
    {
        TIFFErrorExtR(tif, module, "HTJ2K encoding failed: %s", e.what());
        return 0;
    }
    catch (...)
    {
        TIFFErrorExtR(tif, module, "HTJ2K encoding failed: unknown error");
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

static int HTJ2KSetupDecode(TIFF *tif)
{
    HTJ2KState *sp = GetHTJ2KState(tif);
    if (sp->state & HTJ2KSTATE_INIT_ENCODE)
        sp->state = 0;
    sp->state |= HTJ2KSTATE_INIT_DECODE;
    return 1;
}

static int HTJ2KPreDecode(TIFF *tif, uint16_t s)
{
    static const char module[] = "HTJ2KPreDecode";
    HTJ2KState *sp = GetHTJ2KState(tif);
    (void)s;
    if (sp->state != HTJ2KSTATE_INIT_DECODE)
        tif->tif_setupdecode(tif);
    if (!HTJ2KCheckConfiguration(tif, module))
        return 0;
    if (!HTJ2KSetupBuffers(tif))
        return 0;
    sp->accum_used = 0;
    sp->accum_read = 0;

    /* A subsampled component is decoded into a compact buffer, then
     * nearest-neighbour upsampled into the full component plane. */
    const int needs_sub_buffer =
        (tif->tif_dir.td_planarconfig == PLANARCONFIG_SEPARATE &&
         (sp->ds_x != 1 || sp->ds_y != 1));
    if (needs_sub_buffer)
    {
        tmsize_t sub_size = (tmsize_t)sp->cs_width * sp->cs_height *
                            sp->sample_bytes;
        if (sp->sub_buffer == NULL ||
            sp->sub_buffer_capacity < sub_size)
        {
            if (sp->sub_buffer)
                _TIFFfreeExt(tif, sp->sub_buffer);
            sp->sub_buffer =
                (uint8_t *)_TIFFmallocExt(tif, sub_size);
            if (sp->sub_buffer == NULL)
            {
                sp->sub_buffer_capacity = 0;
                TIFFErrorExtR(tif, module,
                              "Cannot allocate HTJ2K subsampling buffer");
                return 0;
            }
            sp->sub_buffer_capacity = sub_size;
        }
    }

    if (tif->tif_rawcc <= 0)
    {
        TIFFErrorExtR(tif, module, "Empty HTJ2K codestream in strip/tile");
        return 0;
    }

    HTJ2KInstallHandlers();
    HTJ2KActiveGuard guard(tif);

    try
    {
        ojph::codestream cs;
        cs.enable_resilience();
        ojph::mem_infile infile;
        infile.open((const ojph::ui8 *)tif->tif_rawcp,
                    (size_t)tif->tif_rawcc);
        cs.read_headers(&infile);

        ojph::param_siz siz = cs.access_siz();
        const ojph::ui32 ncomp = siz.get_num_components();
        if (ncomp != sp->ncomponents)
        {
            TIFFErrorExtR(tif, module,
                          "HTJ2K codestream has %u component(s), TIFF "
                          "strip/tile expects %u",
                          (unsigned)ncomp, (unsigned)sp->ncomponents);
            return 0;
        }
        for (ojph::ui32 c = 0; c < ncomp; ++c)
        {
            ojph::point ds = siz.get_downsampling(c);
            if ((uint32_t)ds.x != sp->ds_x || (uint32_t)ds.y != sp->ds_y)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K component %u subsampling (%d,%d) does "
                              "not match TIFF (%u,%u)",
                              (unsigned)c, (int)ds.x, (int)ds.y,
                              (unsigned)sp->ds_x, (unsigned)sp->ds_y);
                return 0;
            }
            if (siz.get_bit_depth(c) != tif->tif_dir.td_bitspersample ||
                siz.is_signed(c) != (sp->is_signed != 0))
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K component %u bit depth/signedness "
                              "(%u,%d) does not match TIFF (%u,%d)",
                              (unsigned)c, (unsigned)siz.get_bit_depth(c),
                              (int)siz.is_signed(c),
                              (unsigned)tif->tif_dir.td_bitspersample,
                              sp->is_signed);
                return 0;
            }
            if (siz.get_recon_width(c) != sp->cs_width ||
                siz.get_recon_height(c) != sp->cs_height)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K component %u dimensions %ux%u do not "
                              "match expected %ux%u",
                              (unsigned)c,
                              (unsigned)siz.get_recon_width(c),
                              (unsigned)siz.get_recon_height(c),
                              (unsigned)sp->cs_width,
                              (unsigned)sp->cs_height);
                return 0;
            }
        }

        cs.create();

        /* Zero the whole strile so unused edge samples are deterministic. */
        _TIFFmemset(sp->accum, 0, (size_t)sp->accum_capacity);

        const bool planar = (tif->tif_dir.td_planarconfig ==
                             PLANARCONFIG_SEPARATE);
        /* Same layout convention as the encoder: padded tiles, tight
         * strips. */
        const uint32_t stride_w =
            sp->padded_layout ? sp->nom_width : sp->seg_width;
        const uint32_t row_stride =
            stride_w * sp->ncomponents * sp->sample_bytes;
        uint32_t lines_done = 0;
        const uint32_t expected_lines =
            planar ? sp->cs_height * sp->ncomponents
                   : sp->seg_height * sp->ncomponents;

        for (uint32_t i = 0; i < expected_lines; ++i)
        {
            ojph::ui32 comp = 0;
            ojph::line_buf *line = cs.pull(comp);
            if (line == NULL)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K codestream ended after %u lines, "
                              "expected %u",
                              lines_done, expected_lines);
                return 0;
            }
            if (comp >= sp->ncomponents)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K codestream returned invalid component "
                              "index %u",
                              (unsigned)comp);
                return 0;
            }

            uint8_t *dst;
            if (planar)
            {
                /* Single component.  A subsampled component is decoded into
                 * the compact sub-buffer and upsampled later; unit-sampled
                 * components go straight into the accumulation buffer using
                 * its (possibly padded) row stride. */
                if (needs_sub_buffer)
                {
                    dst = sp->sub_buffer +
                          (tmsize_t)i * sp->cs_width * sp->sample_bytes;
                }
                else
                {
                    dst = sp->accum +
                          (tmsize_t)i * stride_w * sp->sample_bytes;
                }
                HTJ2KStoreLine(line->i32, dst, sp->cs_width,
                               sp->sample_bytes, sp->is_signed);
            }
            else
            {
                /* Interleaved: component rows alternate per image row. */
                uint32_t y = i / sp->ncomponents;
                dst = sp->accum + (tmsize_t)y * row_stride;
                if (sp->sample_bytes == 1)
                {
                    if (sp->is_signed)
                    {
                        int8_t *d = (int8_t *)dst;
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            d[x * sp->ncomponents + comp] =
                                (int8_t)line->i32[x];
                    }
                    else
                    {
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            dst[x * sp->ncomponents + comp] =
                                (uint8_t)line->i32[x];
                    }
                }
                else
                {
                    if (sp->is_signed)
                    {
                        int16_t *d = (int16_t *)dst;
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            d[x * sp->ncomponents + comp] =
                                (int16_t)line->i32[x];
                    }
                    else
                    {
                        uint16_t *d = (uint16_t *)dst;
                        for (uint32_t x = 0; x < sp->seg_width; ++x)
                            d[x * sp->ncomponents + comp] =
                                (uint16_t)line->i32[x];
                    }
                }
            }
            ++lines_done;
        }
        cs.close();

        if (lines_done != expected_lines)
        {
            TIFFErrorExtR(tif, module, "Short HTJ2K codestream");
            return 0;
        }

        if (needs_sub_buffer)
        {
            /* Nearest-neighbour upsample the compact component into the
             * full-resolution TIFF plane.  Walk backwards so source samples
             * are not overwritten before they are reused. */
            const uint32_t out_w = sp->padded_layout ? sp->nom_width
                                                      : sp->seg_width;
            const uint32_t out_h = sp->padded_layout ? sp->nom_height
                                                      : sp->seg_height;
            const uint16_t b = sp->sample_bytes;
            for (int32_t oy = (int32_t)out_h - 1; oy >= 0; --oy)
            {
                uint32_t sy = (uint32_t)oy / sp->ds_y;
                if (sy >= sp->cs_height)
                    sy = sp->cs_height - 1;
                for (int32_t ox = (int32_t)out_w - 1; ox >= 0; --ox)
                {
                    uint32_t sx = (uint32_t)ox / sp->ds_x;
                    if (sx >= sp->cs_width)
                        sx = sp->cs_width - 1;
                    uint8_t *d = sp->accum +
                                  ((tmsize_t)oy * out_w + ox) * b;
                    const uint8_t *s =
                        sp->sub_buffer +
                        ((tmsize_t)sy * sp->cs_width + sx) * b;
                    _TIFFmemcpy(d, s, b);
                }
            }
            /* The upsample fills the full plane presented to the TIFF
             * layer: nominal padded size for tiles, tight actual size for
             * strips. */
            sp->accum_used = sp->padded_layout ? sp->accum_capacity
                                                : sp->accum_size;
        }
        else
        {
            /* Unit-sampling layout: nominal padded strile for tiles,
             * tightly packed actual strile for the final strip. */
            sp->accum_used = sp->padded_layout ? sp->accum_capacity
                                                : sp->accum_size;
        }
        sp->accum_read = 0;
        return 1;
    }
    catch (const std::exception &e)
    {
        TIFFErrorExtR(tif, module, "HTJ2K decoding failed: %s", e.what());
        return 0;
    }
    catch (...)
    {
        TIFFErrorExtR(tif, module, "HTJ2K decoding failed: unknown error");
        return 0;
    }
}

static int HTJ2KDecode(TIFF *tif, uint8_t *op, tmsize_t occ, uint16_t s)
{
    static const char module[] = "HTJ2KDecode";
    HTJ2KState *sp = GetHTJ2KState(tif);
    (void)s;

    if (sp->accum == NULL)
    {
        _TIFFmemset(op, 0, (size_t)occ);
        TIFFErrorExtR(tif, module, "HTJ2K buffer not allocated");
        return 0;
    }
    /* The TIFF layer can request a full nominal (padded) strile even when a
     * subsampled component only provides accum_used valid bytes; hand back
     * whatever decoded data is available and zero the tail. */
    tmsize_t available = sp->accum_used - sp->accum_read;
    if (available < 0)
        available = 0;
    tmsize_t to_copy = occ < available ? occ : available;
    if (to_copy > 0)
        _TIFFmemcpy(op, sp->accum + sp->accum_read, (size_t)to_copy);
    if (to_copy < occ)
        _TIFFmemset(op + to_copy, 0, (size_t)(occ - to_copy));
    sp->accum_read += to_copy;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Codec-specific pseudo tags                                          */
/* ------------------------------------------------------------------ */

static int HTJ2KVSetField(TIFF *tif, uint32_t tag, va_list ap)
{
    static const char module[] = "HTJ2KVSetField";
    HTJ2KState *sp = GetHTJ2KState(tif);

    switch (tag)
    {
        case TIFFTAG_HTJ2K_LOSSLESS:
            sp->reversible = va_arg(ap, int) ? 1 : 0;
            return 1;
        case TIFFTAG_HTJ2K_NUMDECOMP:
        {
            int v = va_arg(ap, int);
            if (v < 0 || v > 32)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K decomposition levels must be in [0,32], "
                              "got %d",
                              v);
                return 0;
            }
            sp->num_decomps = v;
            return 1;
        }
        case TIFFTAG_HTJ2K_QSTEP:
        {
            double v = va_arg(ap, double);
            if (!(v > 0.0) || v > 1.0)
            {
                TIFFErrorExtR(tif, module,
                              "HTJ2K quantization step must be in (0,1], "
                              "got %g",
                              v);
                return 0;
            }
            sp->qstep = v;
            return 1;
        }
        default:
            return (*sp->vsetparent)(tif, tag, ap);
    }
}

static int HTJ2KVGetField(TIFF *tif, uint32_t tag, va_list ap)
{
    HTJ2KState *sp = GetHTJ2KState(tif);
    switch (tag)
    {
        case TIFFTAG_HTJ2K_LOSSLESS:
            *va_arg(ap, int *) = sp->reversible;
            break;
        case TIFFTAG_HTJ2K_NUMDECOMP:
            *va_arg(ap, int *) = sp->num_decomps;
            break;
        case TIFFTAG_HTJ2K_QSTEP:
            *va_arg(ap, double *) = sp->qstep;
            break;
        default:
            return (*sp->vgetparent)(tif, tag, ap);
    }
    return 1;
}

static const TIFFField HTJ2KFields[] = {
    {TIFFTAG_HTJ2K_LOSSLESS, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT,
     FIELD_PSEUDO, TRUE, FALSE, (char *)"HTJ2K lossless mode", NULL},
    {TIFFTAG_HTJ2K_NUMDECOMP, 0, 0, TIFF_ANY, 0, TIFF_SETGET_INT,
     FIELD_PSEUDO, TRUE, FALSE,
     (char *)"HTJ2K wavelet decomposition levels", NULL},
    {TIFFTAG_HTJ2K_QSTEP, 0, 0, TIFF_ANY, 0, TIFF_SETGET_DOUBLE,
     FIELD_PSEUDO, TRUE, FALSE,
     (char *)"HTJ2K lossy quantization step", NULL},
};

static void HTJ2KCleanup(TIFF *tif)
{
    HTJ2KState *sp = GetHTJ2KState(tif);
    assert(sp != NULL);

    tif->tif_tagmethods.vgetfield = sp->vgetparent;
    tif->tif_tagmethods.vsetfield = sp->vsetparent;

    if (sp->accum)
    {
        _TIFFfreeExt(tif, sp->accum);
        sp->accum = NULL;
    }
    if (sp->sub_buffer)
    {
        _TIFFfreeExt(tif, sp->sub_buffer);
        sp->sub_buffer = NULL;
        sp->sub_buffer_capacity = 0;
    }
    _TIFFfreeExt(tif, sp);
    tif->tif_data = NULL;

    _TIFFSetDefaultCompressionState(tif);
}

int TIFFInitHTJ2K(TIFF *tif, int scheme)
{
    static const char module[] = "TIFFInitHTJ2K";
    HTJ2KState *sp;

    assert(scheme == COMPRESSION_HTJ2K);
    (void)scheme;

    if (!_TIFFMergeFields(tif, HTJ2KFields, TIFFArrayCount(HTJ2KFields)))
    {
        TIFFErrorExtR(tif, module,
                      "Merging HTJ2K codec-specific tags failed");
        return 0;
    }

    tif->tif_data = (uint8_t *)_TIFFcallocExt(tif, 1, sizeof(HTJ2KState));
    if (tif->tif_data == NULL)
        goto bad;
    sp = GetHTJ2KState(tif);

    sp->vgetparent = tif->tif_tagmethods.vgetfield;
    tif->tif_tagmethods.vgetfield = HTJ2KVGetField;
    sp->vsetparent = tif->tif_tagmethods.vsetfield;
    tif->tif_tagmethods.vsetfield = HTJ2KVSetField;

    /* Defaults: lossless, 5 wavelet levels (OpenJPH convention). */
    sp->reversible = 1;
    sp->num_decomps = 5;
    sp->qstep = 0.0039;
    sp->state = 0;
    sp->accum = NULL;
    sp->accum_size = 0;
    sp->accum_capacity = 0;
    sp->accum_used = 0;
    sp->sub_buffer = NULL;
    sp->sub_buffer_capacity = 0;

    tif->tif_setupdecode = HTJ2KSetupDecode;
    tif->tif_predecode = HTJ2KPreDecode;
    tif->tif_decoderow = HTJ2KDecode;
    tif->tif_decodestrip = HTJ2KDecode;
    tif->tif_decodetile = HTJ2KDecode;
    tif->tif_setupencode = HTJ2KSetupEncode;
    tif->tif_preencode = HTJ2KPreEncode;
    tif->tif_encoderow = HTJ2KEncode;
    tif->tif_encodestrip = HTJ2KEncode;
    tif->tif_encodetile = HTJ2KEncode;
    tif->tif_postencode = HTJ2KPostEncode;
    tif->tif_cleanup = HTJ2KCleanup;

    tif->tif_decodestatus = TRUE;
    tif->tif_encodestatus = TRUE;

    return 1;

bad:
    TIFFErrorExtR(tif, module, "Cannot allocate HTJ2K state block");
    _TIFFSetDefaultCompressionState(tif);
    return 0;
}

#endif /* HTJ2K_SUPPORT */

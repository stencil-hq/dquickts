/*
 * qbun:image — the native pipeline of Bun.Image (internal:bun/image).
 *
 * run(input, options, callback) queues decode → transform → encode on the
 * libuv threadpool and calls callback(err, data, format, width, height)
 * on the loop thread; runSync(input, options) runs the same job on the
 * calling thread and returns { data, format, width, height } (used for a
 * Bun.Image given as a Response body).
 *
 * input: a file path (read off-thread, 256 MiB cap) or bytes (copied when
 * the job is created). options (built by internal:bun/image):
 *   mode            0 metadata, 1 encode, 2 placeholder (ThumbHash PNG)
 *   maxPixels       decompression-bomb guard (header and resize output)
 *   autoOrient      apply the JPEG EXIF orientation first
 *   rotate          0 | 90 | 180 | 270 (clockwise)
 *   flip, flop      mirror vertically / horizontally
 *   resize          boolean; width, height (0 = keep aspect), filter
 *                   (0 nearest 1 box 2 bilinear 3 cubic 4 mitchell
 *                   5 lanczos2 6 lanczos3 7 mks2013 8 mks2021), inside,
 *                   withoutEnlargement
 *   modulate        boolean; brightness, saturation
 *   format          "" (source format) | jpeg | png | webp | heic | avif
 *   quality, progressive, compressionLevel, palette, colors, dither,
 *   lossless        encoder settings (-1 = default)
 *
 * Order of operations (fixed, like Bun): autoOrient → rotate → flip/flop
 * → resize → modulate. Resampling works on straight (non-premultiplied)
 * alpha, like Bun. Errors carry Bun's stable codes (ERR_IMAGE_*,
 * ERR_OUT_OF_MEMORY) or the system error of a file input.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <gif_lib.h>

#include "qbun.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define IMG_PI       3.14159265358979323846
#define IMG_MAX_FILE (256u * 1024u * 1024u)
#define IMG_MAX_ICC  16707345u

enum { M_METADATA, M_ENCODE, M_PLACEHOLDER };
enum { F_JPEG, F_PNG, F_WEBP, F_HEIC, F_AVIF, F_BMP, F_TIFF, F_GIF, F_COUNT };
static const char *const img_format_names[F_COUNT] = {
    "jpeg", "png", "webp", "heic", "avif", "bmp", "tiff", "gif",
};

enum {
    K_NEAREST,
    K_BOX,
    K_BILINEAR,
    K_CUBIC,
    K_MITCHELL,
    K_LANCZOS2,
    K_LANCZOS3,
    K_MKS2013,
    K_MKS2021,
    K_COUNT
};

enum { E_NONE, E_UNSUPPORTED, E_PIXELS, E_DECODE, E_ENCODE, E_UNKNOWN, E_OOM, E_SYS };
static const char *const img_error_codes[] = {
    "",
    "ERR_IMAGE_FORMAT_UNSUPPORTED",
    "ERR_IMAGE_TOO_MANY_PIXELS",
    "ERR_IMAGE_DECODE_FAILED",
    "ERR_IMAGE_ENCODE_FAILED",
    "ERR_IMAGE_UNKNOWN_FORMAT",
    "ERR_OUT_OF_MEMORY",
    "",
};

typedef struct {
    int kind;
    int sys_err;         /* libuv error of E_SYS */
    const char *syscall; /* E_SYS */
    char msg[256];
} ImgError;

static int img_fail(ImgError *e, int kind, const char *fmt, ...)
{
    va_list ap;
    if (e->kind == E_NONE) {
        e->kind = kind;
        va_start(ap, fmt);
        vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int img_oom(ImgError *e)
{
    return img_fail(e, E_OOM, "Out of memory");
}

/* 8-bit RGBA pixels, plus the ICC profile of the source */
typedef struct {
    uint8_t *px;
    uint32_t w, h;
    uint8_t *icc;
    size_t icc_len;
} Bitmap;

static void bitmap_free(Bitmap *b)
{
    free(b->px);
    free(b->icc);
    memset(b, 0, sizeof(*b));
}

static uint8_t *img_alloc_px(ImgError *e, uint32_t w, uint32_t h, int zero)
{
    uint8_t *p;
    if (!w || !h) {
        img_fail(e, E_DECODE, "Image has a zero dimension");
        return NULL;
    }
    if ((uint64_t)w * h > SIZE_MAX / 4) {
        img_fail(e, E_PIXELS, "Image of %ux%u pixels is too large", w, h);
        return NULL;
    }
    p = zero ? calloc((size_t)w * h, 4) : malloc((size_t)w * h * 4);
    if (!p)
        img_oom(e);
    return p;
}

static int bitmap_has_alpha(const Bitmap *b)
{
    size_t i, n = (size_t)b->w * b->h * 4;
    for (i = 3; i < n; i += 4) {
        if (b->px[i] != 255)
            return 1;
    }
    return 0;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd_le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t rd_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd16(const uint8_t *p, int le)
{
    return le ? rd_le16(p) : (((uint32_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p, int le)
{
    return le ? rd_le32(p) : rd_be32(p);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
    p[2] = (v >> 16) & 255;
    p[3] = (v >> 24) & 255;
}

/* ------------------------------------------------------------------ */
/* format sniffing and headers */

static int is_heif_brand(const uint8_t *b)
{
    static const char *const brands[] = {
        "heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "hevs", "mif1", "msf1",
    };
    size_t i;
    for (i = 0; i < countof(brands); i++) {
        if (!memcmp(b, brands[i], 4))
            return 1;
    }
    return 0;
}

static int img_sniff(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return F_JPEG;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return F_PNG;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return F_WEBP;
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
        return F_GIF;
    if (n >= 26 && d[0] == 'B' && d[1] == 'M')
        return F_BMP;
    if (n >= 8 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4)))
        return F_TIFF;
    if (n >= 16 && !memcmp(d + 4, "ftyp", 4)) {
        size_t end = rd_be32(d), i;
        int heif = 0;
        if (end < 16 || end > n)
            end = n < 256 ? n : 256;
        if (!memcmp(d + 8, "avif", 4) || !memcmp(d + 8, "avis", 4))
            return F_AVIF;
        if (is_heif_brand(d + 8) && memcmp(d + 8, "mif1", 4) && memcmp(d + 8, "msf1", 4))
            return F_HEIC;
        heif = is_heif_brand(d + 8);
        for (i = 16; i + 4 <= end; i += 4) {
            if (!memcmp(d + i, "avif", 4) || !memcmp(d + i, "avis", 4))
                return F_AVIF;
            if (is_heif_brand(d + i))
                heif = 1;
        }
        if (heif)
            return F_HEIC;
    }
    return -1;
}

typedef struct {
    uint32_t w, h;
    int orientation; /* EXIF orientation 1..8 (JPEG) */
} ImgInfo;

/* the Orientation tag of an APP1 "Exif\0\0" payload, 1 when absent */
static int exif_orientation(const uint8_t *d, size_t n)
{
    uint32_t ifd, count, i, type, v;
    int le;
    if (n < 14 || memcmp(d, "Exif\0\0", 6))
        return 1;
    d += 6;
    n -= 6;
    if (d[0] == 'I' && d[1] == 'I')
        le = 1;
    else if (d[0] == 'M' && d[1] == 'M')
        le = 0;
    else
        return 1;
    ifd = rd32(d + 4, le);
    if (ifd < 8 || (size_t)ifd + 2 > n)
        return 1;
    count = rd16(d + ifd, le);
    for (i = 0; i < count; i++) {
        size_t off = (size_t)ifd + 2 + (size_t)i * 12;
        if (off + 12 > n)
            break;
        if (rd16(d + off, le) != 0x0112)
            continue;
        type = rd16(d + off + 2, le);
        if (type == 3)
            v = rd16(d + off + 8, le);
        else if (type == 4)
            v = rd32(d + off + 8, le);
        else
            v = 1;
        return v >= 1 && v <= 8 ? (int)v : 1;
    }
    return 1;
}

static int qpng_probe(ImgError *e, const uint8_t *d, size_t n, ImgInfo *info)
{
    if (n < 24 || memcmp(d + 12, "IHDR", 4))
        return img_fail(e, E_DECODE, "Failed to decode PNG: missing IHDR chunk");
    info->w = rd_be32(d + 16);
    info->h = rd_be32(d + 20);
    if (!info->w || !info->h || info->w > 0x7fffffffu || info->h > 0x7fffffffu)
        return img_fail(e, E_DECODE, "Failed to decode PNG: invalid image size");
    return 0;
}

static int gif_probe(ImgError *e, const uint8_t *d, size_t n, ImgInfo *info)
{
    if (n < 13)
        return img_fail(e, E_DECODE, "Failed to decode GIF: truncated header");
    info->w = rd_le16(d + 6);
    info->h = rd_le16(d + 8);
    if (!info->w || !info->h)
        return img_fail(e, E_DECODE, "Failed to decode GIF: invalid screen size");
    return 0;
}

static int webp_probe(ImgError *e, const uint8_t *d, size_t n, ImgInfo *info)
{
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK || f.width <= 0 || f.height <= 0)
        return img_fail(e, E_DECODE, "Failed to decode WebP: invalid header");
    info->w = (uint32_t)f.width;
    info->h = (uint32_t)f.height;
    return 0;
}

static int tiff_probe(ImgError *e, const uint8_t *d, size_t n, ImgInfo *info)
{
    int le = d[0] == 'I';
    uint32_t ifd = rd32(d + 4, le), count, i, w = 0, h = 0;
    if ((size_t)ifd + 2 > n)
        return img_fail(e, E_DECODE, "Failed to decode TIFF: truncated header");
    count = rd16(d + ifd, le);
    for (i = 0; i < count; i++) {
        size_t off = (size_t)ifd + 2 + (size_t)i * 12;
        uint32_t tag, type, v;
        if (off + 12 > n)
            break;
        tag = rd16(d + off, le);
        type = rd16(d + off + 2, le);
        v = type == 3 ? rd16(d + off + 8, le) : type == 4 ? rd32(d + off + 8, le) : 0;
        if (tag == 256)
            w = v;
        else if (tag == 257)
            h = v;
    }
    if (!w || !h)
        return img_fail(e, E_DECODE, "Failed to decode TIFF: missing image size");
    info->w = w;
    info->h = h;
    return 0;
}

/* HEIF/AVIF: the largest 'ispe' (image spatial extents) property */
static int heif_probe(ImgError *e, const uint8_t *d, size_t n, int fmt, ImgInfo *info)
{
    size_t i, end = n < (4u << 20) ? n : (4u << 20);
    uint64_t best = 0;
    for (i = 4; i + 16 <= end; i++) {
        uint32_t w, h;
        if (d[i] != 'i' || memcmp(d + i, "ispe", 4) || rd_be32(d + i - 4) < 20)
            continue;
        w = rd_be32(d + i + 8);
        h = rd_be32(d + i + 12);
        if ((uint64_t)w * h > best) {
            best = (uint64_t)w * h;
            info->w = w;
            info->h = h;
        }
    }
    if (!best)
        return img_fail(e, E_UNSUPPORTED, "Reading %s images is not supported on this platform",
                        img_format_names[fmt]);
    return 0;
}

typedef struct {
    uint32_t off, dib, bpp, comp, clr_used, pal_off, pal_entry;
    uint32_t mask[4];
    int32_t w, h;
    int top_down;
} BmpHeader;

static int bmp_header(ImgError *e, const uint8_t *d, size_t n, BmpHeader *bh)
{
    memset(bh, 0, sizeof(*bh));
    bh->off = rd_le32(d + 10);
    bh->dib = rd_le32(d + 14);
    if (bh->dib == 12) {
        bh->w = (int32_t)rd_le16(d + 18);
        bh->h = (int16_t)rd_le16(d + 20);
        bh->bpp = rd_le16(d + 24);
        bh->pal_entry = 3;
    } else if (bh->dib >= 40 && n >= 54) {
        bh->w = (int32_t)rd_le32(d + 18);
        bh->h = (int32_t)rd_le32(d + 22);
        bh->bpp = rd_le16(d + 28);
        bh->comp = rd_le32(d + 30);
        bh->clr_used = rd_le32(d + 46);
        bh->pal_entry = 4;
        if (bh->comp == 3 || bh->comp == 6) {
            if (n < 66)
                return img_fail(e, E_DECODE, "Failed to decode BMP: truncated header");
            bh->mask[0] = rd_le32(d + 54);
            bh->mask[1] = rd_le32(d + 58);
            bh->mask[2] = rd_le32(d + 62);
            if ((bh->dib >= 56 || bh->comp == 6) && n >= 70)
                bh->mask[3] = rd_le32(d + 66);
        } else if (bh->dib >= 56 && n >= 70 && bh->bpp == 32) {
            bh->mask[3] = 0;
        }
    } else {
        return img_fail(e, E_DECODE, "Failed to decode BMP: unsupported header");
    }
    bh->pal_off = 14 + bh->dib;
    if (bh->dib == 40 && bh->comp == 3)
        bh->pal_off += 12;
    else if (bh->dib == 40 && bh->comp == 6)
        bh->pal_off += 16;
    if (bh->w <= 0 || bh->h == 0 || bh->h == INT32_MIN)
        return img_fail(e, E_DECODE, "Failed to decode BMP: invalid image size");
    bh->top_down = bh->h < 0;
    if (bh->h < 0)
        bh->h = -bh->h;
    return 0;
}

static int bmp_probe(ImgError *e, const uint8_t *d, size_t n, ImgInfo *info)
{
    BmpHeader bh;
    if (bmp_header(e, d, n, &bh))
        return -1;
    info->w = (uint32_t)bh.w;
    info->h = (uint32_t)bh.h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* JPEG (libjpeg-turbo) */

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
} JpegErr;

static void qjpeg_on_error(j_common_ptr ci)
{
    JpegErr *je = (JpegErr *)ci->err;
    longjmp(je->jb, 1);
}

/* warnings (corrupt data, premature end of data) do not fail a decode:
   libjpeg-turbo fills the missing blocks like Bun */
static void qjpeg_on_message(j_common_ptr ci, int level)
{
    (void)ci;
    (void)level;
}

static void qjpeg_on_output(j_common_ptr ci)
{
    (void)ci;
}

static void qjpeg_init_err(JpegErr *je)
{
    jpeg_std_error(&je->pub);
    je->pub.error_exit = qjpeg_on_error;
    je->pub.emit_message = qjpeg_on_message;
    je->pub.output_message = qjpeg_on_output;
}

/* the state lives in the caller's frame so that it stays valid after a
   longjmp() back into qjpeg_dec_run() */
typedef struct {
    struct jpeg_decompress_struct ci;
    JpegErr je;
    int created;
    const uint8_t *data;
    size_t len;
    int scale;   /* M of the M/8 IDCT scale */
    ImgInfo *info;
    Bitmap *out; /* NULL: header only */
} JpegDec;

static int qjpeg_dec_run(JpegDec *s)
{
    jpeg_saved_marker_ptr m;
    JSAMPROW row;
    int cmyk;
    if (setjmp(s->je.jb))
        return -1;
    jpeg_create_decompress(&s->ci);
    s->created = 1;
    jpeg_mem_src(&s->ci, s->data, (unsigned long)s->len);
    jpeg_save_markers(&s->ci, JPEG_APP0 + 1, 0xFFFF);
    if (s->out)
        jpeg_save_markers(&s->ci, JPEG_APP0 + 2, 0xFFFF);
    jpeg_read_header(&s->ci, TRUE);
    s->info->w = s->ci.image_width;
    s->info->h = s->ci.image_height;
    s->info->orientation = 1;
    for (m = s->ci.marker_list; m; m = m->next) {
        if (m->marker == JPEG_APP0 + 1 && m->data_length >= 14 && !memcmp(m->data, "Exif\0\0", 6)) {
            s->info->orientation = exif_orientation(m->data, m->data_length);
            break;
        }
    }
    if (!s->out)
        return 0;
    {
        JOCTET *icc = NULL;
        unsigned int icc_len = 0;
        if (jpeg_read_icc_profile(&s->ci, &icc, &icc_len) && icc) {
            s->out->icc = icc;
            s->out->icc_len = icc_len;
        }
    }
    cmyk = s->ci.jpeg_color_space == JCS_CMYK || s->ci.jpeg_color_space == JCS_YCCK;
    s->ci.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_RGBA;
    s->ci.scale_num = (unsigned int)s->scale;
    s->ci.scale_denom = 8;
    s->ci.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&s->ci);
    if (s->ci.output_components != 4)
        return 1;
    s->out->w = s->ci.output_width;
    s->out->h = s->ci.output_height;
    s->out->px = malloc((size_t)s->out->w * s->out->h * 4);
    if (!s->out->px)
        return 2;
    while (s->ci.output_scanline < s->ci.output_height) {
        row = s->out->px + (size_t)s->ci.output_scanline * s->out->w * 4;
        if (jpeg_read_scanlines(&s->ci, &row, 1) != 1)
            break;
    }
    if (s->ci.output_scanline < s->ci.output_height) {
        /* cannot happen with a memory source; keep the rows grey */
        size_t done = (size_t)s->ci.output_scanline * s->out->w * 4;
        memset(s->out->px + done, 128, (size_t)s->out->w * s->out->h * 4 - done);
    }
    if (cmyk) {
        size_t i, n = (size_t)s->out->w * s->out->h * 4;
        int inverted = s->ci.saw_Adobe_marker;
        uint8_t *p = s->out->px;
        for (i = 0; i < n; i += 4) {
            unsigned c = p[i], mm = p[i + 1], y = p[i + 2], k = p[i + 3];
            if (!inverted) {
                c = 255 - c;
                mm = 255 - mm;
                y = 255 - y;
                k = 255 - k;
            }
            p[i] = (uint8_t)((c * k + 127) / 255);
            p[i + 1] = (uint8_t)((mm * k + 127) / 255);
            p[i + 2] = (uint8_t)((y * k + 127) / 255);
            p[i + 3] = 255;
        }
    }
    return 0;
}

static int qjpeg_decode(ImgError *e, const uint8_t *d, size_t n, int scale, ImgInfo *info,
                        Bitmap *out)
{
    JpegDec s;
    int r;
    memset(&s, 0, sizeof(s));
    qjpeg_init_err(&s.je);
    s.ci.err = &s.je.pub;
    s.data = d;
    s.len = n;
    s.scale = scale;
    s.info = info;
    s.out = out;
    r = qjpeg_dec_run(&s);
    if (r < 0) {
        char msg[JMSG_LENGTH_MAX];
        msg[0] = '\0';
        (*s.je.pub.format_message)((j_common_ptr)&s.ci, msg);
        img_fail(e, E_DECODE, "Failed to decode JPEG: %s", msg);
    } else if (r == 1) {
        img_fail(e, E_DECODE, "Failed to decode JPEG: unsupported color space");
    } else if (r == 2) {
        img_oom(e);
    }
    if (s.created)
        jpeg_destroy_decompress(&s.ci);
    return r ? -1 : 0;
}

typedef struct {
    struct jpeg_compress_struct ci;
    JpegErr je;
    int created;
    unsigned char *buf;
    unsigned long size;
    const Bitmap *b;
    int quality, progressive;
} JpegEnc;

static int qjpeg_enc_run(JpegEnc *s)
{
    JSAMPROW row;
    if (setjmp(s->je.jb))
        return -1;
    jpeg_create_compress(&s->ci);
    s->created = 1;
    jpeg_mem_dest(&s->ci, &s->buf, &s->size);
    s->ci.image_width = s->b->w;
    s->ci.image_height = s->b->h;
    s->ci.input_components = 4;
    s->ci.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&s->ci);
    jpeg_set_quality(&s->ci, s->quality, TRUE);
    if (s->progressive)
        jpeg_simple_progression(&s->ci);
    jpeg_start_compress(&s->ci, TRUE);
    if (s->b->icc && s->b->icc_len)
        jpeg_write_icc_profile(&s->ci, s->b->icc, (unsigned int)s->b->icc_len);
    while (s->ci.next_scanline < s->ci.image_height) {
        row = (JSAMPROW)(s->b->px + (size_t)s->ci.next_scanline * s->b->w * 4);
        jpeg_write_scanlines(&s->ci, &row, 1);
    }
    jpeg_finish_compress(&s->ci);
    return 0;
}

static int qjpeg_encode(ImgError *e, const Bitmap *b, int quality, int progressive, uint8_t **out,
                        size_t *out_len)
{
    JpegEnc s;
    int r;
    if (b->w > 65500 || b->h > 65500)
        return img_fail(e, E_ENCODE, "Failed to encode JPEG: %ux%u exceeds the 65500 pixel limit",
                        b->w, b->h);
    memset(&s, 0, sizeof(s));
    qjpeg_init_err(&s.je);
    s.ci.err = &s.je.pub;
    s.b = b;
    s.quality = quality;
    s.progressive = progressive;
    r = qjpeg_enc_run(&s);
    if (r < 0) {
        char msg[JMSG_LENGTH_MAX];
        msg[0] = '\0';
        (*s.je.pub.format_message)((j_common_ptr)&s.ci, msg);
        img_fail(e, E_ENCODE, "Failed to encode JPEG: %s", msg);
    }
    if (s.created)
        jpeg_destroy_compress(&s.ci);
    if (r < 0) {
        free(s.buf);
        return -1;
    }
    *out = s.buf;
    *out_len = s.size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PNG (libpng) */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} MemReader;

typedef struct {
    uint8_t *data;
    size_t len, cap;
} MemWriter;

static int mw_append(MemWriter *w, const uint8_t *d, size_t n)
{
    if (n > w->cap - w->len) {
        size_t cap = w->cap ? w->cap : 65536;
        uint8_t *p;
        while (cap - w->len < n) {
            if (cap > SIZE_MAX / 2)
                return -1;
            cap *= 2;
        }
        p = realloc(w->data, cap);
        if (!p)
            return -1;
        w->data = p;
        w->cap = cap;
    }
    memcpy(w->data + w->len, d, n);
    w->len += n;
    return 0;
}

typedef struct {
    char msg[200];
    int oom;
} PngErr;

static void qpng_on_error(png_structp png, png_const_charp msg)
{
    PngErr *pe = png_get_error_ptr(png);
    snprintf(pe->msg, sizeof(pe->msg), "%s", msg ? msg : "unknown error");
    png_longjmp(png, 1);
}

static void qpng_on_warning(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static void qpng_on_read(png_structp png, png_bytep out, size_t n)
{
    MemReader *r = png_get_io_ptr(png);
    if (n > r->len - r->pos)
        png_error(png, "unexpected end of PNG data");
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
}

static void qpng_on_write(png_structp png, png_bytep d, size_t n)
{
    MemWriter *w = png_get_io_ptr(png);
    if (mw_append(w, d, n)) {
        ((PngErr *)png_get_error_ptr(png))->oom = 1;
        png_error(png, "out of memory");
    }
}

static void qpng_on_flush(png_structp png)
{
    (void)png;
}

typedef struct {
    png_structp png;
    png_infop info;
    PngErr pe;
    MemReader rd;
    png_bytep *rows;
    Bitmap *out;
} PngDec;

static int qpng_dec_run(PngDec *s)
{
    png_uint_32 w, h, y;
    int depth, ctype, interlace, trns;
    if (setjmp(png_jmpbuf(s->png)))
        return -1;
    png_set_read_fn(s->png, &s->rd, qpng_on_read);
    /* like Bun: neither chunk CRCs nor the zlib Adler-32 are verified */
    png_set_crc_action(s->png, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
#if defined(PNG_SET_OPTION_SUPPORTED) && defined(PNG_IGNORE_ADLER32)
    png_set_option(s->png, PNG_IGNORE_ADLER32, PNG_OPTION_ON);
#endif
#ifdef PNG_BENIGN_ERRORS_SUPPORTED
    png_set_benign_errors(s->png, 1);
#endif
#ifdef PNG_SET_USER_LIMITS_SUPPORTED
    png_set_user_limits(s->png, 0x7fffffffU, 0x7fffffffU);
#endif
    png_read_info(s->png, s->info);
    png_get_IHDR(s->png, s->info, &w, &h, &depth, &ctype, &interlace, NULL, NULL);
    trns = png_get_valid(s->png, s->info, PNG_INFO_tRNS) != 0;
    if (ctype == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(s->png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(s->png);
    if (trns)
        png_set_tRNS_to_alpha(s->png);
    if (depth == 16) {
#ifdef PNG_READ_SCALE_16_TO_8_SUPPORTED
        png_set_scale_16(s->png);
#else
        png_set_strip_16(s->png);
#endif
    }
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(s->png);
    if (!(ctype & PNG_COLOR_MASK_ALPHA) && !trns)
        png_set_add_alpha(s->png, 0xFF, PNG_FILLER_AFTER);
    (void)png_set_interlace_handling(s->png);
    png_read_update_info(s->png, s->info);
    if (png_get_rowbytes(s->png, s->info) != (size_t)w * 4)
        png_error(s->png, "unsupported pixel layout");
    {
        png_charp name;
        int comp;
        png_bytep prof;
        png_uint_32 plen;
        if (png_get_iCCP(s->png, s->info, &name, &comp, &prof, &plen) && prof && plen &&
            plen <= IMG_MAX_ICC) {
            s->out->icc = malloc(plen);
            if (s->out->icc) {
                memcpy(s->out->icc, prof, plen);
                s->out->icc_len = plen;
            }
        }
    }
    if ((uint64_t)w * h > SIZE_MAX / 4)
        return 2;
    s->out->px = malloc((size_t)w * h * 4);
    s->rows = malloc((size_t)h * sizeof(png_bytep));
    if (!s->out->px || !s->rows)
        return 3;
    for (y = 0; y < h; y++)
        s->rows[y] = s->out->px + (size_t)y * w * 4;
    /* png_read_end() is not called: like Bun, what follows the image data
       is ignored */
    png_read_image(s->png, s->rows);
    s->out->w = w;
    s->out->h = h;
    return 0;
}

static int qpng_decode(ImgError *e, const uint8_t *d, size_t n, Bitmap *out)
{
    PngDec s;
    int r;
    memset(&s, 0, sizeof(s));
    s.rd.data = d;
    s.rd.len = n;
    s.out = out;
    s.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &s.pe, qpng_on_error, qpng_on_warning);
    if (!s.png)
        return img_oom(e);
    s.info = png_create_info_struct(s.png);
    if (!s.info) {
        png_destroy_read_struct(&s.png, NULL, NULL);
        return img_oom(e);
    }
    r = qpng_dec_run(&s);
    if (r < 0)
        img_fail(e, E_DECODE, "Failed to decode PNG: %s", s.pe.msg);
    else if (r == 2)
        img_fail(e, E_PIXELS, "PNG image is too large");
    else if (r == 3)
        img_oom(e);
    free(s.rows);
    png_destroy_read_struct(&s.png, &s.info, NULL);
    return r ? -1 : 0;
}

typedef struct {
    png_structp png;
    png_infop info;
    PngErr pe;
    MemWriter mw;
    png_bytep *rows;
    uint32_t w, h;
    int depth, ctype, level;
    png_color plte[256];
    png_byte trns[256];
    int nplte, ntrns;
    const uint8_t *icc;
    size_t icc_len;
} PngEnc;

static int qpng_enc_run(PngEnc *s)
{
    if (setjmp(png_jmpbuf(s->png)))
        return -1;
    png_set_write_fn(s->png, &s->mw, qpng_on_write, qpng_on_flush);
#ifdef PNG_BENIGN_ERRORS_SUPPORTED
    /* an invalid ICC profile is dropped instead of failing the encode */
    png_set_benign_errors(s->png, 1);
#endif
    png_set_compression_level(s->png, s->level);
    if (s->level == 0 || s->ctype == PNG_COLOR_TYPE_PALETTE)
        png_set_filter(s->png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    png_set_IHDR(s->png, s->info, s->w, s->h, s->depth, s->ctype, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (s->ctype == PNG_COLOR_TYPE_PALETTE) {
        png_set_PLTE(s->png, s->info, s->plte, s->nplte);
        if (s->ntrns > 0)
            png_set_tRNS(s->png, s->info, s->trns, s->ntrns, NULL);
    }
    if (s->icc && s->icc_len && s->icc_len <= 0xffffffffu)
        png_set_iCCP(s->png, s->info, "ICC Profile", PNG_COMPRESSION_TYPE_BASE, s->icc,
                     (png_uint_32)s->icc_len);
    png_write_info(s->png, s->info);
    if (s->depth < 8)
        png_set_packing(s->png);
    png_write_image(s->png, s->rows);
    png_write_end(s->png, NULL);
    return 0;
}

/* palette quantization: exact when the image has at most 'colors' colors,
   median cut over a 5-bit-per-channel RGBA histogram otherwise */
typedef struct {
    uint8_t pal[256][4];
    int n;
    uint8_t *idx; /* one palette index per pixel */
} Quant;

#define Q_BITS 5
#define Q_KEY(r, g, b, a)                                                                          \
    ((((uint32_t)(r) >> 3) << 15) | (((uint32_t)(g) >> 3) << 10) | (((uint32_t)(b) >> 3) << 5) |   \
     ((uint32_t)(a) >> 3))
#define Q_CH(key, c) (((key) >> (15 - 5 * (c))) & 31)

typedef struct {
    uint32_t key, count;
} QBucket;

typedef struct {
    size_t lo, hi; /* bucket range */
    uint64_t pop;
    int range, axis;
} QBox;

static void qbox_measure(QBox *box, const QBucket *bk)
{
    int c, lo[4] = {31, 31, 31, 31}, hi[4] = {0, 0, 0, 0};
    size_t i;
    box->pop = 0;
    for (i = box->lo; i < box->hi; i++) {
        box->pop += bk[i].count;
        for (c = 0; c < 4; c++) {
            int v = (int)Q_CH(bk[i].key, c);
            if (v < lo[c])
                lo[c] = v;
            if (v > hi[c])
                hi[c] = v;
        }
    }
    box->range = -1;
    box->axis = 0;
    for (c = 0; c < 4; c++) {
        if (hi[c] - lo[c] > box->range) {
            box->range = hi[c] - lo[c];
            box->axis = c;
        }
    }
}

static int quant_nearest(const Quant *q, int r, int g, int b, int a)
{
    int i, best = 0;
    long bd = LONG_MAX;
    for (i = 0; i < q->n; i++) {
        long dr = r - q->pal[i][0], dg = g - q->pal[i][1], db = b - q->pal[i][2],
             da = a - q->pal[i][3];
        long dist = dr * dr + dg * dg + db * db + da * da;
        if (dist < bd) {
            bd = dist;
            best = i;
        }
    }
    return best;
}

static int quantize_exact(const Bitmap *b, int colors, Quant *q)
{
    uint32_t keys[1024];
    int16_t vals[1024];
    uint8_t used[1024];
    uint32_t order[256];
    size_t i, n = (size_t)b->w * b->h;
    int k, count = 0, ntrans = 0;
    memset(used, 0, sizeof(used));
    for (i = 0; i < n; i++) {
        uint32_t c;
        unsigned hsh;
        memcpy(&c, b->px + i * 4, 4);
        hsh = (c * 2654435761u) >> 22;
        while (used[hsh] && keys[hsh] != c)
            hsh = (hsh + 1) & 1023;
        if (!used[hsh]) {
            if (count == colors)
                return 0;
            used[hsh] = 1;
            keys[hsh] = c;
            order[count++] = c;
        }
    }
    /* translucent entries first, so that tRNS stays short */
    q->n = 0;
    for (k = 0; k < count; k++) {
        if (((const uint8_t *)&order[k])[3] != 255)
            memcpy(q->pal[q->n++], &order[k], 4), ntrans++;
    }
    for (k = 0; k < count; k++) {
        if (((const uint8_t *)&order[k])[3] == 255)
            memcpy(q->pal[q->n++], &order[k], 4);
    }
    for (k = 0; k < q->n; k++) {
        uint32_t c;
        unsigned hsh;
        memcpy(&c, q->pal[k], 4);
        hsh = (c * 2654435761u) >> 22;
        while (keys[hsh] != c)
            hsh = (hsh + 1) & 1023;
        vals[hsh] = (int16_t)k;
    }
    for (i = 0; i < n; i++) {
        uint32_t c;
        unsigned hsh;
        memcpy(&c, b->px + i * 4, 4);
        hsh = (c * 2654435761u) >> 22;
        while (keys[hsh] != c)
            hsh = (hsh + 1) & 1023;
        q->idx[i] = (uint8_t)vals[hsh];
    }
    (void)ntrans;
    return 1;
}

static int quantize(ImgError *e, const Bitmap *b, int colors, int dither, Quant *q)
{
    size_t npx = (size_t)b->w * b->h, i, nb = 0;
    uint32_t *hist = NULL;
    QBucket *bk = NULL, *tmp = NULL;
    int16_t *cache = NULL;
    QBox boxes[256];
    int nbox, k, c, ret = -1;
    float *err = NULL;

    q->idx = malloc(npx);
    if (!q->idx)
        return img_oom(e);
    if (quantize_exact(b, colors, q))
        return 0;

    hist = calloc((size_t)1 << 20, sizeof(uint32_t));
    if (!hist)
        goto oom;
    for (i = 0; i < npx; i++) {
        const uint8_t *p = b->px + i * 4;
        hist[Q_KEY(p[0], p[1], p[2], p[3])]++;
    }
    for (i = 0; i < ((size_t)1 << 20); i++)
        nb += hist[i] != 0;
    bk = malloc(nb * sizeof(QBucket));
    tmp = malloc(nb * sizeof(QBucket));
    if (!bk || !tmp)
        goto oom;
    nb = 0;
    for (i = 0; i < ((size_t)1 << 20); i++) {
        if (hist[i]) {
            bk[nb].key = (uint32_t)i;
            bk[nb].count = hist[i];
            nb++;
        }
    }
    free(hist);
    hist = NULL;

    boxes[0].lo = 0;
    boxes[0].hi = nb;
    qbox_measure(&boxes[0], bk);
    nbox = 1;
    while (nbox < colors) {
        int best = -1;
        size_t cnt[32], pos[32], j;
        uint64_t half, acc;
        QBox *bx;
        for (k = 0; k < nbox; k++) {
            if (boxes[k].hi - boxes[k].lo < 2 || boxes[k].range <= 0)
                continue;
            if (best < 0 || boxes[k].range > boxes[best].range ||
                (boxes[k].range == boxes[best].range && boxes[k].pop > boxes[best].pop))
                best = k;
        }
        if (best < 0)
            break;
        bx = &boxes[best];
        /* counting sort of the box along its widest channel */
        memset(cnt, 0, sizeof(cnt));
        for (j = bx->lo; j < bx->hi; j++)
            cnt[Q_CH(bk[j].key, bx->axis)]++;
        pos[0] = bx->lo;
        for (c = 1; c < 32; c++)
            pos[c] = pos[c - 1] + cnt[c - 1];
        for (j = bx->lo; j < bx->hi; j++)
            tmp[pos[Q_CH(bk[j].key, bx->axis)]++] = bk[j];
        memcpy(bk + bx->lo, tmp + bx->lo, (bx->hi - bx->lo) * sizeof(QBucket));
        /* split at the population median, between distinct values */
        half = bx->pop / 2;
        acc = 0;
        for (j = bx->lo; j < bx->hi - 1; j++) {
            acc += bk[j].count;
            if (acc >= half && Q_CH(bk[j].key, bx->axis) != Q_CH(bk[j + 1].key, bx->axis))
                break;
        }
        if (j >= bx->hi - 1) {
            /* every bucket past the median shares the value: split at the
               last value change */
            for (j = bx->hi - 1; j > bx->lo; j--) {
                if (Q_CH(bk[j - 1].key, bx->axis) != Q_CH(bk[j].key, bx->axis))
                    break;
            }
            j--;
        }
        boxes[nbox].lo = j + 1;
        boxes[nbox].hi = bx->hi;
        bx->hi = j + 1;
        qbox_measure(bx, bk);
        qbox_measure(&boxes[nbox], bk);
        nbox++;
    }
    /* palette: population-weighted mean of each box; translucent first */
    {
        uint8_t raw[256][4];
        int nt = 0;
        for (k = 0; k < nbox; k++) {
            double sum[4] = {0, 0, 0, 0};
            size_t j;
            for (j = boxes[k].lo; j < boxes[k].hi; j++) {
                for (c = 0; c < 4; c++) {
                    uint32_t v = Q_CH(bk[j].key, c);
                    sum[c] += (double)((v << 3) | (v >> 2)) * bk[j].count;
                }
            }
            for (c = 0; c < 4; c++)
                raw[k][c] = (uint8_t)floor(sum[c] / (double)boxes[k].pop + 0.5);
        }
        q->n = 0;
        for (k = 0; k < nbox; k++) {
            if (raw[k][3] != 255)
                memcpy(q->pal[q->n++], raw[k], 4), nt++;
        }
        for (k = 0; k < nbox; k++) {
            if (raw[k][3] == 255)
                memcpy(q->pal[q->n++], raw[k], 4);
        }
        (void)nt;
    }
    free(bk);
    free(tmp);
    bk = tmp = NULL;

    cache = malloc(((size_t)1 << 20) * sizeof(int16_t));
    if (!cache)
        goto oom;
    memset(cache, 0xff, ((size_t)1 << 20) * sizeof(int16_t));
    if (!dither) {
        for (i = 0; i < npx; i++) {
            const uint8_t *p = b->px + i * 4;
            uint32_t key = Q_KEY(p[0], p[1], p[2], p[3]);
            if (cache[key] < 0)
                cache[key] = (int16_t)quant_nearest(q, p[0], p[1], p[2], p[3]);
            q->idx[i] = (uint8_t)cache[key];
        }
    } else {
        /* Floyd–Steinberg error diffusion */
        size_t row = ((size_t)b->w + 2) * 4;
        uint32_t x, y;
        err = calloc(row * 2, sizeof(float));
        if (!err)
            goto oom;
        for (y = 0; y < b->h; y++) {
            float *cur = err + (y & 1) * row, *next = err + ((y + 1) & 1) * row;
            memset(next, 0, row * sizeof(float));
            for (x = 0; x < b->w; x++) {
                const uint8_t *p = b->px + ((size_t)y * b->w + x) * 4;
                int v[4], id;
                uint32_t key;
                for (c = 0; c < 4; c++) {
                    float f = p[c] + cur[(x + 1) * 4 + c];
                    v[c] = f < 0 ? 0 : f > 255 ? 255 : (int)(f + 0.5f);
                }
                key = Q_KEY(v[0], v[1], v[2], v[3]);
                if (cache[key] < 0)
                    cache[key] = (int16_t)quant_nearest(q, v[0], v[1], v[2], v[3]);
                id = cache[key];
                q->idx[(size_t)y * b->w + x] = (uint8_t)id;
                for (c = 0; c < 4; c++) {
                    float d = (float)(v[c] - q->pal[id][c]);
                    cur[(x + 2) * 4 + c] += d * (7.0f / 16);
                    next[x * 4 + c] += d * (3.0f / 16);
                    next[(x + 1) * 4 + c] += d * (5.0f / 16);
                    next[(x + 2) * 4 + c] += d * (1.0f / 16);
                }
            }
        }
    }
    ret = 0;
    goto done;
oom:
    img_oom(e);
done:
    free(hist);
    free(bk);
    free(tmp);
    free(cache);
    free(err);
    return ret;
}

static int qpng_encode(ImgError *e, const Bitmap *b, int level, int palette, int colors, int dither,
                       uint8_t **out, size_t *out_len)
{
    PngEnc s;
    uint8_t *packed = NULL;
    Quant q;
    size_t stride, y;
    int r;

    memset(&s, 0, sizeof(s));
    memset(&q, 0, sizeof(q));
    s.w = b->w;
    s.h = b->h;
    s.level = level;
    s.icc = b->icc;
    s.icc_len = b->icc_len;
    s.depth = 8;
    if (palette) {
        int k;
        if (quantize(e, b, colors, dither, &q)) {
            free(q.idx);
            return -1;
        }
        s.ctype = PNG_COLOR_TYPE_PALETTE;
        s.nplte = q.n;
        for (k = 0; k < q.n; k++) {
            s.plte[k].red = q.pal[k][0];
            s.plte[k].green = q.pal[k][1];
            s.plte[k].blue = q.pal[k][2];
            s.trns[k] = q.pal[k][3];
            if (q.pal[k][3] != 255)
                s.ntrns = k + 1;
        }
        s.depth = q.n <= 2 ? 1 : q.n <= 4 ? 2 : q.n <= 16 ? 4 : 8;
        packed = q.idx;
        stride = b->w;
    } else if (bitmap_has_alpha(b)) {
        s.ctype = PNG_COLOR_TYPE_RGBA;
        stride = (size_t)b->w * 4;
    } else {
        size_t i, n = (size_t)b->w * b->h;
        s.ctype = PNG_COLOR_TYPE_RGB;
        stride = (size_t)b->w * 3;
        packed = malloc(n * 3);
        if (!packed)
            return img_oom(e);
        for (i = 0; i < n; i++) {
            packed[i * 3] = b->px[i * 4];
            packed[i * 3 + 1] = b->px[i * 4 + 1];
            packed[i * 3 + 2] = b->px[i * 4 + 2];
        }
    }
    s.rows = malloc((size_t)b->h * sizeof(png_bytep));
    if (!s.rows) {
        free(packed);
        return img_oom(e);
    }
    for (y = 0; y < b->h; y++)
        s.rows[y] = (packed ? packed : b->px) + y * stride;
    s.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &s.pe, qpng_on_error, qpng_on_warning);
    s.info = s.png ? png_create_info_struct(s.png) : NULL;
    if (!s.info) {
        if (s.png)
            png_destroy_write_struct(&s.png, NULL);
        free(s.rows);
        free(packed);
        return img_oom(e);
    }
    r = qpng_enc_run(&s);
    png_destroy_write_struct(&s.png, &s.info);
    free(s.rows);
    free(packed);
    if (r) {
        free(s.mw.data);
        if (s.pe.oom)
            return img_oom(e);
        return img_fail(e, E_ENCODE, "Failed to encode PNG: %s", s.pe.msg);
    }
    *out = s.mw.data;
    *out_len = s.mw.len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* WebP (libwebp, libwebpdemux) */

static void webp_read_icc(const uint8_t *d, size_t n, Bitmap *out)
{
    WebPData wd;
    WebPDemuxer *dmux;
    WebPChunkIterator it;
    wd.bytes = d;
    wd.size = n;
    dmux = WebPDemux(&wd);
    if (!dmux)
        return;
    if (WebPDemuxGetChunk(dmux, "ICCP", 1, &it)) {
        if (it.chunk.size && it.chunk.size <= IMG_MAX_ICC) {
            out->icc = malloc(it.chunk.size);
            if (out->icc) {
                memcpy(out->icc, it.chunk.bytes, it.chunk.size);
                out->icc_len = it.chunk.size;
            }
        }
        WebPDemuxReleaseChunkIterator(&it);
    }
    WebPDemuxDelete(dmux);
}

static int webp_decode(ImgError *e, const uint8_t *d, size_t n, Bitmap *out)
{
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK)
        return img_fail(e, E_DECODE, "Failed to decode WebP: invalid header");
    if (f.has_animation) {
        WebPAnimDecoderOptions o;
        WebPAnimDecoder *dec;
        WebPAnimInfo ai;
        WebPData wd;
        uint8_t *buf;
        int ts;
        if (!WebPAnimDecoderOptionsInit(&o))
            return img_fail(e, E_DECODE, "Failed to decode WebP: library version mismatch");
        o.color_mode = MODE_RGBA;
        o.use_threads = 0;
        wd.bytes = d;
        wd.size = n;
        dec = WebPAnimDecoderNew(&wd, &o);
        if (!dec)
            return img_fail(e, E_DECODE, "Failed to decode WebP: invalid animation");
        if (!WebPAnimDecoderGetInfo(dec, &ai) || !WebPAnimDecoderGetNext(dec, &buf, &ts)) {
            WebPAnimDecoderDelete(dec);
            return img_fail(e, E_DECODE, "Failed to decode WebP: invalid animation frame");
        }
        out->px = img_alloc_px(e, ai.canvas_width, ai.canvas_height, 0);
        if (!out->px) {
            WebPAnimDecoderDelete(dec);
            return -1;
        }
        out->w = ai.canvas_width;
        out->h = ai.canvas_height;
        memcpy(out->px, buf, (size_t)out->w * out->h * 4);
        WebPAnimDecoderDelete(dec);
    } else {
        size_t size;
        out->px = img_alloc_px(e, (uint32_t)f.width, (uint32_t)f.height, 0);
        if (!out->px)
            return -1;
        out->w = (uint32_t)f.width;
        out->h = (uint32_t)f.height;
        size = (size_t)out->w * out->h * 4;
        if (!WebPDecodeRGBAInto(d, n, out->px, size, (int)(out->w * 4)))
            return img_fail(e, E_DECODE, "Failed to decode WebP: corrupt or truncated data");
    }
    webp_read_icc(d, n, out);
    return 0;
}

/* re-mux a WebP with a VP8X header and an ICCP chunk (libwebpmux is not
   linked) */
static int webp_add_icc(ImgError *e, const uint8_t *src, size_t n, const Bitmap *b, int has_alpha,
                        uint8_t **out, size_t *out_len)
{
    size_t pos, total = 12 + 18 + 8 + b->icc_len + (b->icc_len & 1), o;
    uint8_t flags = 0, *p;
    for (pos = 12; pos + 8 <= n;) {
        uint32_t size = rd_le32(src + pos + 4);
        size_t csize = 8 + (size_t)size + (size & 1);
        if (csize > n - pos)
            csize = n - pos;
        if (!memcmp(src + pos, "VP8X", 4))
            flags = pos + 8 < n ? src[pos + 8] : 0;
        else
            total += csize;
        pos += csize;
    }
    if (total - 8 > 0xfffffff0u)
        return img_fail(e, E_ENCODE, "Failed to encode WebP: output too large");
    p = malloc(total);
    if (!p)
        return img_oom(e);
    memcpy(p, "RIFF", 4);
    wr_le32(p + 4, (uint32_t)(total - 8));
    memcpy(p + 8, "WEBPVP8X", 8);
    wr_le32(p + 16, 10);
    flags |= 0x20;
    if (has_alpha)
        flags |= 0x10;
    p[20] = flags;
    p[21] = p[22] = p[23] = 0;
    p[24] = (b->w - 1) & 255;
    p[25] = ((b->w - 1) >> 8) & 255;
    p[26] = ((b->w - 1) >> 16) & 255;
    p[27] = (b->h - 1) & 255;
    p[28] = ((b->h - 1) >> 8) & 255;
    p[29] = ((b->h - 1) >> 16) & 255;
    memcpy(p + 30, "ICCP", 4);
    wr_le32(p + 34, (uint32_t)b->icc_len);
    memcpy(p + 38, b->icc, b->icc_len);
    o = 38 + b->icc_len;
    if (b->icc_len & 1)
        p[o++] = 0;
    for (pos = 12; pos + 8 <= n;) {
        uint32_t size = rd_le32(src + pos + 4);
        size_t csize = 8 + (size_t)size + (size & 1);
        if (csize > n - pos)
            csize = n - pos;
        if (memcmp(src + pos, "VP8X", 4)) {
            memcpy(p + o, src + pos, csize);
            o += csize;
        }
        pos += csize;
    }
    *out = p;
    *out_len = o;
    return 0;
}

static int webp_encode(ImgError *e, const Bitmap *b, int quality, int lossless, uint8_t **out,
                       size_t *out_len)
{
    WebPConfig cfg;
    WebPPicture pic;
    WebPMemoryWriter wr;
    int has_alpha = bitmap_has_alpha(b), ok, code;
    if (b->w > 16383 || b->h > 16383)
        return img_fail(e, E_ENCODE, "Failed to encode WebP: %ux%u exceeds the 16383 pixel limit",
                        b->w, b->h);
    if (!WebPConfigInit(&cfg) || !WebPPictureInit(&pic))
        return img_fail(e, E_ENCODE, "Failed to encode WebP: library version mismatch");
    cfg.lossless = lossless ? 1 : 0;
    cfg.quality = (float)(quality >= 0 ? quality : lossless ? 75 : 80);
    cfg.method = 4;
    if (!WebPValidateConfig(&cfg))
        return img_fail(e, E_ENCODE, "Failed to encode WebP: invalid configuration");
    pic.use_argb = cfg.lossless;
    pic.width = (int)b->w;
    pic.height = (int)b->h;
    if (!(has_alpha ? WebPPictureImportRGBA(&pic, b->px, (int)(b->w * 4))
                    : WebPPictureImportRGBX(&pic, b->px, (int)(b->w * 4)))) {
        WebPPictureFree(&pic);
        return img_oom(e);
    }
    WebPMemoryWriterInit(&wr);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &wr;
    ok = WebPEncode(&cfg, &pic);
    code = pic.error_code;
    WebPPictureFree(&pic);
    if (!ok) {
        WebPMemoryWriterClear(&wr);
        if (code == VP8_ENC_ERROR_OUT_OF_MEMORY || code == VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY)
            return img_oom(e);
        return img_fail(e, E_ENCODE, "Failed to encode WebP: error %d", code);
    }
    if (b->icc && b->icc_len) {
        ok = webp_add_icc(e, wr.mem, wr.size, b, has_alpha, out, out_len);
        WebPMemoryWriterClear(&wr);
        return ok;
    }
    *out = malloc(wr.size ? wr.size : 1);
    if (!*out) {
        WebPMemoryWriterClear(&wr);
        return img_oom(e);
    }
    memcpy(*out, wr.mem, wr.size);
    *out_len = wr.size;
    WebPMemoryWriterClear(&wr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GIF (giflib, first frame) */

static int gif_on_read(GifFileType *g, GifByteType *buf, int n)
{
    MemReader *r = g->UserData;
    size_t avail = r->len - r->pos;
    if (n < 0)
        return 0;
    if ((size_t)n > avail)
        n = (int)avail;
    memcpy(buf, r->data + r->pos, (size_t)n);
    r->pos += (size_t)n;
    return n;
}

static const char *gif_msg(int err)
{
    const char *m = GifErrorString(err);
    return m ? m : "unknown error";
}

static int gif_decode(ImgError *e, const uint8_t *d, size_t n, Bitmap *out)
{
    static const int ioff[4] = {0, 4, 2, 1}, istep[4] = {8, 8, 4, 2};
    MemReader rd;
    GifFileType *g;
    GifRecordType rt;
    GifByteType *ext;
    GifImageDesc *id;
    ColorMapObject *cm;
    uint8_t *line = NULL;
    int err = 0, code, transparent = -1, ret = -1, pass, y, x;
    uint32_t cw, ch;

    rd.data = d;
    rd.len = n;
    rd.pos = 0;
    g = DGifOpen(&rd, gif_on_read, &err);
    if (!g)
        return img_fail(e, E_DECODE, "Failed to decode GIF: %s", gif_msg(err));
    for (;;) {
        if (DGifGetRecordType(g, &rt) == GIF_ERROR)
            goto gif_error;
        if (rt == EXTENSION_RECORD_TYPE) {
            if (DGifGetExtension(g, &code, &ext) == GIF_ERROR)
                goto gif_error;
            if (code == GRAPHICS_EXT_FUNC_CODE && ext && ext[0] >= 4) {
                GraphicsControlBlock gcb;
                if (DGifExtensionToGCB(ext[0], ext + 1, &gcb) == GIF_OK)
                    transparent = gcb.TransparentColor;
            }
            while (ext) {
                if (DGifGetExtensionNext(g, &ext) == GIF_ERROR)
                    goto gif_error;
            }
        } else if (rt == IMAGE_DESC_RECORD_TYPE) {
            break;
        } else if (rt == TERMINATE_RECORD_TYPE) {
            img_fail(e, E_DECODE, "Failed to decode GIF: no image data");
            goto done;
        }
    }
    if (DGifGetImageDesc(g) == GIF_ERROR)
        goto gif_error;
    id = &g->Image;
    cm = id->ColorMap ? id->ColorMap : g->SColorMap;
    if (!cm || id->Width <= 0 || id->Height <= 0 || id->Left < 0 || id->Top < 0) {
        img_fail(e, E_DECODE, "Failed to decode GIF: invalid image descriptor");
        goto done;
    }
    cw = (uint32_t)g->SWidth;
    ch = (uint32_t)g->SHeight;
    if (!cw || !ch) {
        cw = (uint32_t)(id->Left + id->Width);
        ch = (uint32_t)(id->Top + id->Height);
    }
    out->px = img_alloc_px(e, cw, ch, 1);
    if (!out->px)
        goto done;
    out->w = cw;
    out->h = ch;
    line = malloc((size_t)id->Width);
    if (!line) {
        img_oom(e);
        goto done;
    }
    for (pass = 0; pass < (id->Interlace ? 4 : 1); pass++) {
        int start = id->Interlace ? ioff[pass] : 0, step = id->Interlace ? istep[pass] : 1;
        for (y = start; y < id->Height; y += step) {
            uint32_t yy = (uint32_t)(id->Top + y);
            if (DGifGetLine(g, line, id->Width) == GIF_ERROR)
                goto gif_error;
            if (yy >= ch)
                continue;
            for (x = 0; x < id->Width; x++) {
                uint32_t xx = (uint32_t)(id->Left + x);
                uint8_t *p;
                int c = line[x];
                if (c == transparent || xx >= cw || c >= cm->ColorCount)
                    continue;
                p = out->px + ((size_t)yy * cw + xx) * 4;
                p[0] = cm->Colors[c].Red;
                p[1] = cm->Colors[c].Green;
                p[2] = cm->Colors[c].Blue;
                p[3] = 255;
            }
        }
    }
    ret = 0;
    goto done;
gif_error:
    img_fail(e, E_DECODE, "Failed to decode GIF: %s", gif_msg(g->Error));
done:
    free(line);
    DGifCloseFile(g, &err);
    return ret;
}

/* ------------------------------------------------------------------ */
/* BMP (built-in: 1/4/8-bit palette, 16/24/32-bit, bitfields) */

static unsigned mask_shift(uint32_t m)
{
    unsigned s = 0;
    if (!m)
        return 0;
    while (!(m & 1)) {
        m >>= 1;
        s++;
    }
    return s;
}

static uint8_t mask_get(uint32_t v, uint32_t mask, unsigned shift)
{
    uint32_t max;
    if (!mask)
        return 0;
    max = mask >> shift;
    return (uint8_t)((((v & mask) >> shift) * 255u + max / 2) / max);
}

static int bmp_decode(ImgError *e, const uint8_t *d, size_t n, Bitmap *out)
{
    BmpHeader bh;
    uint64_t stride;
    uint32_t x, y, ncolors = 0, w, h;
    uint32_t mask[4];
    unsigned shift[4];
    int c, use_alpha = 0;
    if (bmp_header(e, d, n, &bh))
        return -1;
    w = (uint32_t)bh.w;
    h = (uint32_t)bh.h;
    if (bh.comp != 0 && !((bh.comp == 3 || bh.comp == 6) && (bh.bpp == 16 || bh.bpp == 32)))
        return img_fail(e, E_DECODE, "Failed to decode BMP: unsupported compression %u", bh.comp);
    if (bh.bpp != 1 && bh.bpp != 4 && bh.bpp != 8 && bh.bpp != 16 && bh.bpp != 24 && bh.bpp != 32)
        return img_fail(e, E_DECODE, "Failed to decode BMP: unsupported bit depth %u", bh.bpp);
    stride = (((uint64_t)w * bh.bpp + 31) / 32) * 4;
    if (bh.off > n || stride * h > n - bh.off)
        return img_fail(e, E_DECODE, "Failed to decode BMP: truncated pixel data");
    if (bh.bpp <= 8) {
        ncolors = bh.clr_used ? bh.clr_used : 1u << bh.bpp;
        if (ncolors > 256)
            ncolors = 256;
        if (bh.pal_off > n || (uint64_t)ncolors * bh.pal_entry > n - bh.pal_off)
            return img_fail(e, E_DECODE, "Failed to decode BMP: truncated palette");
    }
    if (bh.comp == 3 || bh.comp == 6) {
        memcpy(mask, bh.mask, sizeof(mask));
    } else if (bh.bpp == 16) {
        mask[0] = 0x7C00;
        mask[1] = 0x03E0;
        mask[2] = 0x001F;
        mask[3] = 0;
    } else {
        mask[0] = 0x00FF0000;
        mask[1] = 0x0000FF00;
        mask[2] = 0x000000FF;
        mask[3] = bh.bpp == 32 ? 0xFF000000u : 0;
    }
    for (c = 0; c < 4; c++)
        shift[c] = mask_shift(mask[c]);
    if (bh.bpp == 32 && mask[3]) {
        /* BI_RGB alpha is often garbage zeros: use it only when some pixel
           has a non-zero alpha */
        for (y = 0; y < h && !use_alpha; y++) {
            const uint8_t *row = d + bh.off + y * stride;
            for (x = 0; x < w; x++) {
                if (rd_le32(row + x * 4) & mask[3]) {
                    use_alpha = 1;
                    break;
                }
            }
        }
    }
    out->px = img_alloc_px(e, w, h, 0);
    if (!out->px)
        return -1;
    out->w = w;
    out->h = h;
    for (y = 0; y < h; y++) {
        const uint8_t *row = d + bh.off + (uint64_t)(bh.top_down ? y : h - 1 - y) * stride;
        uint8_t *o = out->px + (size_t)y * w * 4;
        for (x = 0; x < w; x++, o += 4) {
            if (bh.bpp <= 8) {
                uint32_t bit = x * bh.bpp,
                         idx = (row[bit >> 3] >> (8 - bh.bpp - (bit & 7))) & ((1u << bh.bpp) - 1);
                if (idx < ncolors) {
                    const uint8_t *pe = d + bh.pal_off + idx * bh.pal_entry;
                    o[0] = pe[2];
                    o[1] = pe[1];
                    o[2] = pe[0];
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = 255;
            } else if (bh.bpp == 24) {
                o[0] = row[x * 3 + 2];
                o[1] = row[x * 3 + 1];
                o[2] = row[x * 3];
                o[3] = 255;
            } else {
                uint32_t v = bh.bpp == 16 ? rd_le16(row + x * 2) : rd_le32(row + x * 4);
                o[0] = mask_get(v, mask[0], shift[0]);
                o[1] = mask_get(v, mask[1], shift[1]);
                o[2] = mask_get(v, mask[2], shift[2]);
                o[3] = use_alpha ? mask_get(v, mask[3], shift[3]) : 255;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dispatch */

static int img_probe(ImgError *e, const uint8_t *d, size_t n, int fmt, ImgInfo *info)
{
    info->w = info->h = 0;
    info->orientation = 1;
    switch (fmt) {
    case F_JPEG:
        return qjpeg_decode(e, d, n, 8, info, NULL);
    case F_PNG:
        return qpng_probe(e, d, n, info);
    case F_WEBP:
        return webp_probe(e, d, n, info);
    case F_GIF:
        return gif_probe(e, d, n, info);
    case F_BMP:
        return bmp_probe(e, d, n, info);
    case F_TIFF:
        return tiff_probe(e, d, n, info);
    default:
        return heif_probe(e, d, n, fmt, info);
    }
}

static int img_decode(ImgError *e, const uint8_t *d, size_t n, int fmt, int scale, Bitmap *out)
{
    ImgInfo info;
    switch (fmt) {
    case F_JPEG:
        return qjpeg_decode(e, d, n, scale, &info, out);
    case F_PNG:
        return qpng_decode(e, d, n, out);
    case F_WEBP:
        return webp_decode(e, d, n, out);
    case F_GIF:
        return gif_decode(e, d, n, out);
    case F_BMP:
        return bmp_decode(e, d, n, out);
    default:
        return img_fail(e, E_UNSUPPORTED, "Decoding %s images is not supported on this platform",
                        img_format_names[fmt]);
    }
}

/* ------------------------------------------------------------------ */
/* geometry: one pass for orientation, rotate, flip and flop */

typedef struct {
    /* output (x, y) → source (ax*x + bx*y + cx, ay*x + by*y + cy) */
    long ax, bx, cx, ay, by, cy;
    uint32_t w, h;
} Xform;

enum { OP_FLOP, OP_FLIP, OP_ROT90, OP_ROT180, OP_ROT270, OP_TRANSPOSE, OP_TRANSVERSE };

static void xf_compose(Xform *t, long p, long q, long r, long s, long u, long v, uint32_t nw,
                       uint32_t nh)
{
    Xform o = *t;
    /* new (x, y) → current (p*x + q*y + r, s*x + u*y + v) → source */
    t->ax = o.ax * p + o.bx * s;
    t->bx = o.ax * q + o.bx * u;
    t->cx = o.ax * r + o.bx * v + o.cx;
    t->ay = o.ay * p + o.by * s;
    t->by = o.ay * q + o.by * u;
    t->cy = o.ay * r + o.by * v + o.cy;
    t->w = nw;
    t->h = nh;
}

static void xf_op(Xform *t, int op)
{
    long W = t->w, H = t->h;
    switch (op) {
    case OP_FLOP:
        xf_compose(t, -1, 0, W - 1, 0, 1, 0, t->w, t->h);
        break;
    case OP_FLIP:
        xf_compose(t, 1, 0, 0, 0, -1, H - 1, t->w, t->h);
        break;
    case OP_ROT90:
        xf_compose(t, 0, 1, 0, -1, 0, H - 1, t->h, t->w);
        break;
    case OP_ROT180:
        xf_compose(t, -1, 0, W - 1, 0, -1, H - 1, t->w, t->h);
        break;
    case OP_ROT270:
        xf_compose(t, 0, -1, W - 1, 1, 0, 0, t->h, t->w);
        break;
    case OP_TRANSPOSE:
        xf_compose(t, 0, 1, 0, 1, 0, 0, t->h, t->w);
        break;
    case OP_TRANSVERSE:
        xf_compose(t, 0, -1, W - 1, -1, 0, H - 1, t->h, t->w);
        break;
    }
}

static void xf_orientation(Xform *t, int orientation)
{
    static const int ops[9] = {-1,           -1,       OP_FLOP,       OP_ROT180, OP_FLIP,
                               OP_TRANSPOSE, OP_ROT90, OP_TRANSVERSE, OP_ROT270};
    if (orientation >= 2 && orientation <= 8)
        xf_op(t, ops[orientation]);
}

static int xf_apply(ImgError *e, const Xform *t, Bitmap *b)
{
    uint8_t *dst;
    uint32_t x, y;
    if (t->ax == 1 && t->bx == 0 && t->cx == 0 && t->ay == 0 && t->by == 1 && t->cy == 0)
        return 0;
    dst = img_alloc_px(e, t->w, t->h, 0);
    if (!dst)
        return -1;
    for (y = 0; y < t->h; y++) {
        long sx = t->bx * (long)y + t->cx, sy = t->by * (long)y + t->cy;
        uint8_t *o = dst + (size_t)y * t->w * 4;
        for (x = 0; x < t->w; x++, sx += t->ax, sy += t->ay, o += 4)
            memcpy(o, b->px + ((size_t)sy * b->w + (size_t)sx) * 4, 4);
    }
    free(b->px);
    b->px = dst;
    b->w = t->w;
    b->h = t->h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* resampling (separable, straight alpha like Bun) */

static double k_sinc(double x)
{
    if (x == 0)
        return 1;
    x *= IMG_PI;
    return sin(x) / x;
}

static double k_bc(double x, double B, double C)
{
    if (x < 1)
        return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) /
               6;
    if (x < 2)
        return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x +
                (8 * B + 24 * C)) /
               6;
    return 0;
}

static const double k_radius[K_COUNT] = {0.5, 0.5, 1, 2, 2, 2, 3, 2.5, 4.5};

static double k_eval(int k, double x)
{
    x = fabs(x);
    switch (k) {
    case K_BOX:
        return x <= 0.5 ? 1 : 0;
    case K_BILINEAR:
        return x < 1 ? 1 - x : 0;
    case K_CUBIC:
        return k_bc(x, 0, 0.5);
    case K_MITCHELL:
        return k_bc(x, 1.0 / 3, 1.0 / 3);
    case K_LANCZOS2:
        return x < 2 ? k_sinc(x) * k_sinc(x / 2) : 0;
    case K_LANCZOS3:
        return x < 3 ? k_sinc(x) * k_sinc(x / 3) : 0;
    case K_MKS2013:
        if (x >= 2.5)
            return 0;
        if (x >= 1.5)
            return (x - 2.5) * (x - 2.5) / -8;
        if (x >= 0.5)
            return (4 * x * x - 11 * x + 7) / 4;
        return 17.0 / 16 - 7 * x * x / 4;
    case K_MKS2021:
        if (x >= 4.5)
            return 0;
        if (x >= 3.5)
            return (4 * x * x - 36 * x + 81) / -1152;
        if (x >= 2.5)
            return (4 * x * x - 27 * x + 45) / 144;
        if (x >= 1.5)
            return (24 * x * x - 113 * x + 130) / -144;
        if (x >= 0.5)
            return (140 * x * x - 379 * x + 239) / 144;
        return 577.0 / 576 - 239 * x * x / 144;
    default:
        return 0;
    }
}

typedef struct {
    int *start, *count;
    float *w;
    int maxn;
} Contribs;

static void contribs_free(Contribs *c)
{
    free(c->start);
    free(c->count);
    free(c->w);
}

static int contribs_build(ImgError *e, Contribs *c, uint32_t src, uint32_t dst, int k)
{
    double scale = (double)src / dst, fs = scale > 1 ? scale : 1, support = k_radius[k] * fs;
    uint32_t i;
    c->maxn = (int)ceil(support * 2) + 2;
    if ((uint32_t)c->maxn > src + 2)
        c->maxn = (int)src + 2;
    c->start = malloc(dst * sizeof(int));
    c->count = malloc(dst * sizeof(int));
    c->w = malloc((size_t)dst * c->maxn * sizeof(float));
    if (!c->start || !c->count || !c->w)
        return img_oom(e);
    for (i = 0; i < dst; i++) {
        double center = (i + 0.5) * scale, sum = 0, wt[1024 + 4], *ws = wt, *heap = NULL;
        long lo = (long)floor(center - support), hi = (long)ceil(center + support), j;
        int n, a, b, m;
        float *out = c->w + (size_t)i * c->maxn;
        if (lo < 0)
            lo = 0;
        if (hi > (long)src)
            hi = (long)src;
        if (hi - lo > c->maxn)
            hi = lo + c->maxn;
        if (hi <= lo) {
            lo = (long)(center < src ? center : src - 1);
            hi = lo + 1;
        }
        n = (int)(hi - lo);
        if (n > (int)(sizeof(wt) / sizeof(wt[0]))) {
            heap = malloc((size_t)n * sizeof(double));
            if (!heap)
                return img_oom(e);
            ws = heap;
        }
        for (j = lo; j < hi; j++) {
            ws[j - lo] = k_eval(k, (j + 0.5 - center) / fs);
            sum += ws[j - lo];
        }
        if (sum == 0) {
            long nearest = (long)center;
            if (nearest < lo)
                nearest = lo;
            if (nearest >= hi)
                nearest = hi - 1;
            memset(ws, 0, (size_t)n * sizeof(double));
            ws[nearest - lo] = 1;
            sum = 1;
        }
        /* trim zero taps */
        a = 0;
        b = n;
        while (a < b - 1 && ws[a] == 0)
            a++;
        while (b - 1 > a && ws[b - 1] == 0)
            b--;
        c->start[i] = (int)(lo + a);
        c->count[i] = b - a;
        for (m = a; m < b; m++)
            out[m - a] = (float)(ws[m] / sum);
        free(heap);
    }
    return 0;
}

static uint8_t clamp_u8(float v)
{
    if (!(v > 0))
        return 0;
    if (v >= 255)
        return 255;
    return (uint8_t)(v + 0.5f);
}

static int resample(ImgError *e, Bitmap *b, uint32_t tw, uint32_t th, int k)
{
    Contribs hx, vy;
    float *ring = NULL;
    uint8_t *dst;
    uint32_t x, y;
    long next = 0;
    int ring_rows, ret = -1;

    dst = img_alloc_px(e, tw, th, 0);
    if (!dst)
        return -1;
    if (k == K_NEAREST) {
        double sx = (double)b->w / tw, sy = (double)b->h / th;
        for (y = 0; y < th; y++) {
            uint32_t yy = (uint32_t)((y + 0.5) * sy);
            if (yy >= b->h)
                yy = b->h - 1;
            for (x = 0; x < tw; x++) {
                uint32_t xx = (uint32_t)((x + 0.5) * sx);
                if (xx >= b->w)
                    xx = b->w - 1;
                memcpy(dst + ((size_t)y * tw + x) * 4, b->px + ((size_t)yy * b->w + xx) * 4, 4);
            }
        }
        free(b->px);
        b->px = dst;
        b->w = tw;
        b->h = th;
        return 0;
    }
    memset(&hx, 0, sizeof(hx));
    memset(&vy, 0, sizeof(vy));
    if (contribs_build(e, &hx, b->w, tw, k) || contribs_build(e, &vy, b->h, th, k))
        goto done;
    ring_rows = vy.maxn < (int)b->h ? vy.maxn : (int)b->h;
    ring = malloc((size_t)ring_rows * tw * 4 * sizeof(float));
    if (!ring) {
        img_oom(e);
        goto done;
    }
    for (y = 0; y < th; y++) {
        int s0 = vy.start[y], n = vy.count[y], t;
        const float *wv = vy.w + (size_t)y * vy.maxn;
        uint8_t *o = dst + (size_t)y * tw * 4;
        /* horizontal pass of the source rows this output row needs */
        while (next < s0 + n) {
            const uint8_t *sp = b->px + (size_t)next * b->w * 4;
            float *rp = ring + (size_t)(next % ring_rows) * tw * 4;
            for (x = 0; x < tw; x++) {
                const float *wh = hx.w + (size_t)x * hx.maxn;
                const uint8_t *p = sp + (size_t)hx.start[x] * 4;
                float r = 0, g = 0, bl = 0, a = 0;
                int cnt = hx.count[x];
                for (t = 0; t < cnt; t++, p += 4) {
                    float w = wh[t];
                    r += w * p[0];
                    g += w * p[1];
                    bl += w * p[2];
                    a += w * p[3];
                }
                rp[x * 4] = r;
                rp[x * 4 + 1] = g;
                rp[x * 4 + 2] = bl;
                rp[x * 4 + 3] = a;
            }
            next++;
        }
        for (x = 0; x < tw; x++) {
            float r = 0, g = 0, bl = 0, a = 0;
            for (t = 0; t < n; t++) {
                const float *rp = ring + (size_t)((s0 + t) % ring_rows) * tw * 4 + (size_t)x * 4;
                float w = wv[t];
                r += w * rp[0];
                g += w * rp[1];
                bl += w * rp[2];
                a += w * rp[3];
            }
            o[x * 4] = clamp_u8(r);
            o[x * 4 + 1] = clamp_u8(g);
            o[x * 4 + 2] = clamp_u8(bl);
            o[x * 4 + 3] = clamp_u8(a);
        }
    }
    free(b->px);
    b->px = dst;
    dst = NULL;
    b->w = tw;
    b->h = th;
    ret = 0;
done:
    free(dst);
    free(ring);
    contribs_free(&hx);
    contribs_free(&vy);
    return ret;
}

static void img_modulate(Bitmap *b, double brightness, double saturation)
{
    size_t i, n = (size_t)b->w * b->h * 4;
    for (i = 0; i < n; i += 4) {
        float r = (float)(b->px[i] * brightness), g = (float)(b->px[i + 1] * brightness);
        float bl = (float)(b->px[i + 2] * brightness);
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * bl, s = (float)saturation;
        b->px[i] = clamp_u8(l + (r - l) * s);
        b->px[i + 1] = clamp_u8(l + (g - l) * s);
        b->px[i + 2] = clamp_u8(l + (bl - l) * s);
    }
}

/* ------------------------------------------------------------------ */
/* ThumbHash (https://github.com/evanw/thumbhash, MIT) */

static int th_round(double x)
{
    return (int)floor(x + 0.5);
}

static void th_encode_channel(const float *ch, int w, int h, int nx, int ny, double *dc, double *ac,
                              int *nac, double *scale)
{
    double fx[100];
    int cx, cy, x, y, n = 0, i;
    *dc = 0;
    *scale = 0;
    for (cy = 0; cy < ny; cy++) {
        for (cx = 0; cx * ny < nx * (ny - cy); cx++) {
            double f = 0;
            for (x = 0; x < w; x++)
                fx[x] = cos(IMG_PI / w * cx * (x + 0.5));
            for (y = 0; y < h; y++) {
                double fy = cos(IMG_PI / h * cy * (y + 0.5));
                for (x = 0; x < w; x++)
                    f += ch[x + y * w] * fx[x] * fy;
            }
            f /= w * h;
            if (cx || cy) {
                ac[n++] = f;
                if (fabs(f) > *scale)
                    *scale = fabs(f);
            } else {
                *dc = f;
            }
        }
    }
    if (*scale)
        for (i = 0; i < n; i++)
            ac[i] = 0.5 + 0.5 / *scale * ac[i];
    *nac = n;
}

/* hash: 64 zeroed bytes */
static int th_encode(ImgError *e, const uint8_t *rgba, int w, int h, uint8_t *hash)
{
    double avg_r = 0, avg_g = 0, avg_b = 0, avg_a = 0;
    double l_dc, p_dc, q_dc, a_dc = 0, l_scale, p_scale, q_scale, a_scale = 0;
    double l_ac[64], p_ac[64], q_ac[64], a_ac[64];
    int nl, np, nq, na = 0, i, j, has_alpha, l_limit, lx, ly, mx, landscape, ac_start, ac_index = 0,
                    list;
    uint32_t header24, header16;
    float *l, *p, *q, *a;
    size_t npx = (size_t)w * h;
    l = malloc(npx * 4 * sizeof(float));
    if (!l)
        return img_oom(e);
    p = l + npx;
    q = p + npx;
    a = q + npx;
    for (i = 0, j = 0; i < (int)npx; i++, j += 4) {
        double alpha = rgba[j + 3] / 255.0;
        avg_r += alpha / 255 * rgba[j];
        avg_g += alpha / 255 * rgba[j + 1];
        avg_b += alpha / 255 * rgba[j + 2];
        avg_a += alpha;
    }
    if (avg_a) {
        avg_r /= avg_a;
        avg_g /= avg_a;
        avg_b /= avg_a;
    }
    has_alpha = avg_a < (double)npx;
    l_limit = has_alpha ? 5 : 7;
    mx = w > h ? w : h;
    lx = th_round((double)l_limit * w / mx);
    ly = th_round((double)l_limit * h / mx);
    if (lx < 1)
        lx = 1;
    if (ly < 1)
        ly = 1;
    for (i = 0, j = 0; i < (int)npx; i++, j += 4) {
        double alpha = rgba[j + 3] / 255.0;
        double r = avg_r * (1 - alpha) + alpha / 255 * rgba[j];
        double g = avg_g * (1 - alpha) + alpha / 255 * rgba[j + 1];
        double b = avg_b * (1 - alpha) + alpha / 255 * rgba[j + 2];
        l[i] = (float)((r + g + b) / 3);
        p[i] = (float)((r + g) / 2 - b);
        q[i] = (float)(r - g);
        a[i] = (float)alpha;
    }
    th_encode_channel(l, w, h, lx > 3 ? lx : 3, ly > 3 ? ly : 3, &l_dc, l_ac, &nl, &l_scale);
    th_encode_channel(p, w, h, 3, 3, &p_dc, p_ac, &np, &p_scale);
    th_encode_channel(q, w, h, 3, 3, &q_dc, q_ac, &nq, &q_scale);
    if (has_alpha)
        th_encode_channel(a, w, h, 5, 5, &a_dc, a_ac, &na, &a_scale);
    free(l);
    landscape = w > h;
    header24 = (uint32_t)th_round(63 * l_dc) | ((uint32_t)th_round(31.5 + 31.5 * p_dc) << 6) |
               ((uint32_t)th_round(31.5 + 31.5 * q_dc) << 12) |
               ((uint32_t)th_round(31 * l_scale) << 18) | ((uint32_t)has_alpha << 23);
    header16 = (uint32_t)(landscape ? ly : lx) | ((uint32_t)th_round(63 * p_scale) << 3) |
               ((uint32_t)th_round(63 * q_scale) << 9) | ((uint32_t)landscape << 15);
    hash[0] = header24 & 255;
    hash[1] = (header24 >> 8) & 255;
    hash[2] = (header24 >> 16) & 255;
    hash[3] = header16 & 255;
    hash[4] = (header16 >> 8) & 255;
    ac_start = has_alpha ? 6 : 5;
    if (has_alpha)
        hash[5] = (uint8_t)(th_round(15 * a_dc) | (th_round(15 * a_scale) << 4));
    for (list = 0; list < (has_alpha ? 4 : 3); list++) {
        const double *ac = list == 0 ? l_ac : list == 1 ? p_ac : list == 2 ? q_ac : a_ac;
        int cnt = list == 0 ? nl : list == 1 ? np : list == 2 ? nq : na;
        for (i = 0; i < cnt; i++, ac_index++) {
            int k = ac_start + (ac_index >> 1);
            if (k < 64)
                hash[k] |= (uint8_t)(th_round(15 * ac[i]) << ((ac_index & 1) << 2));
        }
    }
    return 0;
}

static void th_decode_channel(const uint8_t *hash, int ac_start, int *ac_index, int nx, int ny,
                              double scale, double *ac)
{
    int cx, cy, n = 0;
    for (cy = 0; cy < ny; cy++) {
        for (cx = cy ? 0 : 1; cx * ny < nx * (ny - cy); cx++) {
            int k = ac_start + (*ac_index >> 1);
            int v = k < 64 ? (hash[k] >> ((*ac_index & 1) << 2)) & 15 : 0;
            ac[n++] = (v / 7.5 - 1) * scale;
            (*ac_index)++;
        }
    }
}

/* render a ThumbHash to a ≤32px RGBA bitmap */
static int th_decode(ImgError *e, const uint8_t *hash, Bitmap *out)
{
    uint32_t header24 = hash[0] | ((uint32_t)hash[1] << 8) | ((uint32_t)hash[2] << 16);
    uint32_t header16 = hash[3] | ((uint32_t)hash[4] << 8);
    double l_dc = (header24 & 63) / 63.0, p_dc = ((header24 >> 6) & 63) / 31.5 - 1;
    double q_dc = ((header24 >> 12) & 63) / 31.5 - 1, l_scale = ((header24 >> 18) & 31) / 31.0;
    int has_alpha = (header24 >> 23) & 1;
    double p_scale = ((header16 >> 3) & 63) / 63.0, q_scale = ((header16 >> 9) & 63) / 63.0;
    int landscape = (header16 >> 15) & 1;
    int hlx = landscape ? (has_alpha ? 5 : 7) : (int)(header16 & 7);
    int hly = landscape ? (int)(header16 & 7) : (has_alpha ? 5 : 7);
    int lx = hlx > 3 ? hlx : 3, ly = hly > 3 ? hly : 3;
    double a_dc = has_alpha ? (hash[5] & 15) / 15.0 : 1, a_scale = (hash[5] >> 4) / 15.0;
    double l_ac[64], p_ac[64], q_ac[64], a_ac[64], fx[8], fy[8], ratio;
    int ac_start = has_alpha ? 6 : 5, ac_index = 0, w, h, x, y, cx, cy, jj, nfx, nfy;
    uint8_t *px;

    th_decode_channel(hash, ac_start, &ac_index, lx, ly, l_scale, l_ac);
    th_decode_channel(hash, ac_start, &ac_index, 3, 3, p_scale * 1.25, p_ac);
    th_decode_channel(hash, ac_start, &ac_index, 3, 3, q_scale * 1.25, q_ac);
    if (has_alpha)
        th_decode_channel(hash, ac_start, &ac_index, 5, 5, a_scale, a_ac);
    ratio = hly ? (double)hlx / hly : 1;
    w = th_round(ratio > 1 ? 32 : 32 * ratio);
    h = th_round(ratio > 1 ? 32 / ratio : 32);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    px = img_alloc_px(e, (uint32_t)w, (uint32_t)h, 0);
    if (!px)
        return -1;
    nfx = lx > (has_alpha ? 5 : 3) ? lx : (has_alpha ? 5 : 3);
    nfy = ly > (has_alpha ? 5 : 3) ? ly : (has_alpha ? 5 : 3);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            double l = l_dc, p = p_dc, q = q_dc, a = a_dc, b, r, g;
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            for (cx = 0; cx < nfx; cx++)
                fx[cx] = cos(IMG_PI / w * (x + 0.5) * cx);
            for (cy = 0; cy < nfy; cy++)
                fy[cy] = cos(IMG_PI / h * (y + 0.5) * cy);
            for (cy = 0, jj = 0; cy < ly; cy++) {
                double fy2 = fy[cy] * 2;
                for (cx = cy ? 0 : 1; cx * ly < lx * (ly - cy); cx++, jj++)
                    l += l_ac[jj] * fx[cx] * fy2;
            }
            for (cy = 0, jj = 0; cy < 3; cy++) {
                double fy2 = fy[cy] * 2;
                for (cx = cy ? 0 : 1; cx < 3 - cy; cx++, jj++) {
                    double f = fx[cx] * fy2;
                    p += p_ac[jj] * f;
                    q += q_ac[jj] * f;
                }
            }
            if (has_alpha) {
                for (cy = 0, jj = 0; cy < 5; cy++) {
                    double fy2 = fy[cy] * 2;
                    for (cx = cy ? 0 : 1; cx < 5 - cy; cx++, jj++)
                        a += a_ac[jj] * fx[cx] * fy2;
                }
            }
            b = l - 2.0 / 3 * p;
            r = (3 * l - b + q) / 2;
            g = r - q;
            o[0] = (uint8_t)fmax(0, 255 * fmin(1, r));
            o[1] = (uint8_t)fmax(0, 255 * fmin(1, g));
            o[2] = (uint8_t)fmax(0, 255 * fmin(1, b));
            o[3] = (uint8_t)fmax(0, 255 * fmin(1, a));
        }
    }
    out->px = px;
    out->w = (uint32_t)w;
    out->h = (uint32_t)h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* jobs */

typedef struct QBImgJob {
    uv_work_t work;
    QBRuntime *qr;
    JSValue callback;
    char *path;
    uint8_t *input;
    size_t input_len;
    int mode;
    double max_pixels;
    int auto_orient, rotate, flip, flop;
    int resize, rw, rh, filter, fit_inside, without_enlargement;
    int modulate;
    double brightness, saturation;
    int format, quality, progressive, compression, palette, colors, dither, lossless;
    ImgError err;
    uint8_t *out;
    size_t out_len;
    int out_format;
    uint32_t out_w, out_h;
} QBImgJob;

static void job_free(QBImgJob *j)
{
    free(j->path);
    free(j->input);
    free(j->out);
    free(j);
}

static int job_sys_fail(QBImgJob *j, int errnum, const char *syscall)
{
    if (j->err.kind == E_NONE) {
        j->err.kind = E_SYS;
        j->err.sys_err = uv_translate_sys_error(errnum);
        j->err.syscall = syscall;
    }
    return -1;
}

static int job_read_file(QBImgJob *j)
{
    struct stat st;
    size_t cap, len = 0;
    uint8_t *buf;
    int fd, err;
    fd = open(j->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return job_sys_fail(j, errno, "open");
    if (fstat(fd, &st)) {
        err = errno;
        close(fd);
        return job_sys_fail(j, err, "fstat");
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return job_sys_fail(j, EISDIR, "read");
    }
    if (st.st_size > (off_t)IMG_MAX_FILE) {
        close(fd);
        return img_fail(&j->err, E_PIXELS, "Input file is larger than 256 MiB");
    }
    cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 65536;
    buf = malloc(cap);
    if (!buf) {
        close(fd);
        return img_oom(&j->err);
    }
    for (;;) {
        ssize_t r;
        if (len == cap) {
            uint8_t *p;
            if (cap > IMG_MAX_FILE) {
                free(buf);
                close(fd);
                return img_fail(&j->err, E_PIXELS, "Input file is larger than 256 MiB");
            }
            p = realloc(buf, cap * 2);
            if (!p) {
                free(buf);
                close(fd);
                return img_oom(&j->err);
            }
            buf = p;
            cap *= 2;
        }
        r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            free(buf);
            close(fd);
            return job_sys_fail(j, err, "read");
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    close(fd);
    if (len > IMG_MAX_FILE) {
        free(buf);
        return img_fail(&j->err, E_PIXELS, "Input file is larger than 256 MiB");
    }
    j->input = buf;
    j->input_len = len;
    return 0;
}

static uint32_t dim_round(double v)
{
    v = floor(v + 0.5);
    if (v < 1)
        return 1;
    if (v > 0x7fffffff)
        return 0x7fffffff;
    return (uint32_t)v;
}

static void job_resize_dims(const QBImgJob *j, uint32_t cw, uint32_t ch, uint32_t *ptw,
                            uint32_t *pth)
{
    double rw = j->rw, rh = j->rh;
    uint32_t tw = cw, th = ch;
    if (rw > 0 && rh > 0) {
        if (j->fit_inside) {
            if (rw / cw <= rh / ch) {
                tw = (uint32_t)rw;
                th = dim_round(ch * rw / cw);
            } else {
                th = (uint32_t)rh;
                tw = dim_round(cw * rh / ch);
            }
        } else {
            tw = (uint32_t)rw;
            th = (uint32_t)rh;
        }
    } else if (rw > 0) {
        tw = (uint32_t)rw;
        th = dim_round(ch * rw / cw);
    } else if (rh > 0) {
        th = (uint32_t)rh;
        tw = dim_round(cw * rh / ch);
    }
    if (j->without_enlargement && (tw > cw || th > ch)) {
        tw = cw;
        th = ch;
    }
    *ptw = tw;
    *pth = th;
}

static void job_execute(QBImgJob *j)
{
    ImgError *e = &j->err;
    ImgInfo info;
    Bitmap bm;
    Xform t;
    int fmt, orient, transposed, out_fmt, scale = 8, m, rot_swap;
    uint32_t ow, oh, cw, ch, tw, th;

    memset(&bm, 0, sizeof(bm));
    if (j->path && job_read_file(j))
        return;
    fmt = img_sniff(j->input, j->input_len);
    if (fmt < 0) {
        img_fail(e, E_UNKNOWN, "Unrecognized image format");
        return;
    }
    if (img_probe(e, j->input, j->input_len, fmt, &info))
        return;
    orient = j->auto_orient ? info.orientation : 1;
    ow = orient >= 5 ? info.h : info.w;
    oh = orient >= 5 ? info.w : info.h;
    if ((double)info.w * info.h > j->max_pixels) {
        img_fail(e, E_PIXELS, "Image dimensions %ux%u exceed maxPixels (%.0f)", info.w, info.h,
                 j->max_pixels);
        return;
    }
    if (j->mode == M_METADATA) {
        j->out_format = fmt;
        j->out_w = ow;
        j->out_h = oh;
        return;
    }
    if (fmt == F_HEIC || fmt == F_AVIF || fmt == F_TIFF) {
        img_fail(e, E_UNSUPPORTED, "Decoding %s images is not supported on this platform",
                 img_format_names[fmt]);
        return;
    }
    if (j->mode == M_ENCODE) {
        out_fmt = j->format >= 0                                     ? j->format
                  : (fmt == F_JPEG || fmt == F_PNG || fmt == F_WEBP) ? fmt
                                                                     : F_PNG;
        if (out_fmt == F_HEIC || out_fmt == F_AVIF) {
            img_fail(e, E_UNSUPPORTED, "Encoding %s images is not supported on this platform",
                     img_format_names[out_fmt]);
            return;
        }
    } else {
        out_fmt = F_PNG;
    }

    rot_swap = j->mode == M_ENCODE && (j->rotate == 90 || j->rotate == 270);
    cw = rot_swap ? oh : ow;
    ch = rot_swap ? ow : oh;
    tw = cw;
    th = ch;
    if (j->mode == M_ENCODE && j->resize) {
        job_resize_dims(j, cw, ch, &tw, &th);
    } else if (j->mode == M_PLACEHOLDER && (cw > 100 || ch > 100)) {
        if (cw >= ch) {
            tw = 100;
            th = dim_round(ch * 100.0 / cw);
        } else {
            th = 100;
            tw = dim_round(cw * 100.0 / ch);
        }
    }
    if ((double)tw * th > j->max_pixels) {
        img_fail(e, E_PIXELS, "Resize output %ux%u exceeds maxPixels (%.0f)", tw, th,
                 j->max_pixels);
        return;
    }
    /* JPEG: decode straight at the M/8 IDCT scale nearest above the target
       when it is at most half the source size */
    transposed = (orient >= 5) ^ rot_swap;
    if (fmt == F_JPEG && (tw != cw || th != ch) &&
        (j->mode == M_PLACEHOLDER || j->filter != K_NEAREST)) {
        uint64_t nw = transposed ? th : tw, nh = transposed ? tw : th;
        if (nw * 2 <= info.w && nh * 2 <= info.h) {
            for (m = 1; m < 8; m++) {
                if (((uint64_t)info.w * m + 7) / 8 >= nw && ((uint64_t)info.h * m + 7) / 8 >= nh) {
                    scale = m;
                    break;
                }
            }
        }
    }
    if (img_decode(e, j->input, j->input_len, fmt, scale, &bm))
        goto done;
    free(j->input);
    j->input = NULL;
    j->input_len = 0;

    t.ax = 1;
    t.bx = 0;
    t.cx = 0;
    t.ay = 0;
    t.by = 1;
    t.cy = 0;
    t.w = bm.w;
    t.h = bm.h;
    xf_orientation(&t, orient);
    if (j->mode == M_ENCODE) {
        if (j->rotate == 90)
            xf_op(&t, OP_ROT90);
        else if (j->rotate == 180)
            xf_op(&t, OP_ROT180);
        else if (j->rotate == 270)
            xf_op(&t, OP_ROT270);
        if (j->flip)
            xf_op(&t, OP_FLIP);
        if (j->flop)
            xf_op(&t, OP_FLOP);
    }
    if (xf_apply(e, &t, &bm))
        goto done;
    if (bm.w != tw || bm.h != th) {
        if (resample(e, &bm, tw, th, j->mode == M_PLACEHOLDER ? K_BOX : j->filter))
            goto done;
    }
    if (j->mode == M_PLACEHOLDER) {
        uint8_t hash[64];
        Bitmap ph;
        memset(hash, 0, sizeof(hash));
        memset(&ph, 0, sizeof(ph));
        if (th_encode(e, bm.px, (int)bm.w, (int)bm.h, hash) || th_decode(e, hash, &ph))
            goto done;
        bitmap_free(&bm);
        bm = ph;
        if (qpng_encode(e, &bm, 9, 0, 256, 0, &j->out, &j->out_len))
            goto done;
    } else {
        if (j->modulate)
            img_modulate(&bm, j->brightness, j->saturation);
        if (out_fmt == F_JPEG) {
            if (qjpeg_encode(e, &bm, j->quality >= 0 ? j->quality : 80, j->progressive, &j->out,
                             &j->out_len))
                goto done;
        } else if (out_fmt == F_WEBP) {
            if (webp_encode(e, &bm, j->quality, j->lossless, &j->out, &j->out_len))
                goto done;
        } else {
            if (qpng_encode(e, &bm, j->compression >= 0 ? j->compression : 6, j->palette,
                            j->colors >= 2 ? j->colors : 256, j->dither, &j->out, &j->out_len))
                goto done;
        }
    }
    j->out_format = out_fmt;
    j->out_w = bm.w;
    j->out_h = bm.h;
done:
    bitmap_free(&bm);
}

/* ------------------------------------------------------------------ */
/* JS bindings */

static int opt_num(JSContext *ctx, JSValueConst o, const char *name, double def, double *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, name);
    int r;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *out = def;
        return 0;
    }
    r = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    if (!r && isnan(*out))
        *out = def;
    return r;
}

static int opt_int(JSContext *ctx, JSValueConst o, const char *name, int def, int *out)
{
    double d;
    if (opt_num(ctx, o, name, def, &d))
        return -1;
    if (d > INT_MAX)
        d = INT_MAX;
    if (d < INT_MIN)
        d = INT_MIN;
    *out = (int)d;
    return 0;
}

static int opt_bool(JSContext *ctx, JSValueConst o, const char *name, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, name);
    int r;
    if (JS_IsException(v))
        return -1;
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (r < 0)
        return -1;
    *out = r;
    return 0;
}

static int opt_format(JSContext *ctx, JSValueConst o, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "format");
    const char *s;
    int i;
    if (JS_IsException(v))
        return -1;
    *out = -1;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < F_COUNT; i++) {
        if (!strcmp(s, img_format_names[i]))
            *out = i;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static QBImgJob *job_new(JSContext *ctx, JSValueConst input, JSValueConst opts)
{
    QBImgJob *j = calloc(1, sizeof(*j));
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    j->callback = JS_UNDEFINED;
    j->qr = qb_get(ctx);
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        goto fail;
    }
    if (opt_int(ctx, opts, "mode", M_ENCODE, &j->mode) ||
        opt_num(ctx, opts, "maxPixels", 268402689.0, &j->max_pixels) ||
        opt_bool(ctx, opts, "autoOrient", &j->auto_orient) ||
        opt_int(ctx, opts, "rotate", 0, &j->rotate) || opt_bool(ctx, opts, "flip", &j->flip) ||
        opt_bool(ctx, opts, "flop", &j->flop) || opt_bool(ctx, opts, "resize", &j->resize) ||
        opt_int(ctx, opts, "width", 0, &j->rw) || opt_int(ctx, opts, "height", 0, &j->rh) ||
        opt_int(ctx, opts, "filter", K_LANCZOS3, &j->filter) ||
        opt_bool(ctx, opts, "inside", &j->fit_inside) ||
        opt_bool(ctx, opts, "withoutEnlargement", &j->without_enlargement) ||
        opt_bool(ctx, opts, "modulate", &j->modulate) ||
        opt_num(ctx, opts, "brightness", 1, &j->brightness) ||
        opt_num(ctx, opts, "saturation", 1, &j->saturation) || opt_format(ctx, opts, &j->format) ||
        opt_int(ctx, opts, "quality", -1, &j->quality) ||
        opt_bool(ctx, opts, "progressive", &j->progressive) ||
        opt_int(ctx, opts, "compressionLevel", -1, &j->compression) ||
        opt_bool(ctx, opts, "palette", &j->palette) ||
        opt_int(ctx, opts, "colors", 256, &j->colors) ||
        opt_bool(ctx, opts, "dither", &j->dither) || opt_bool(ctx, opts, "lossless", &j->lossless))
        goto fail;
    if (j->mode < M_METADATA || j->mode > M_PLACEHOLDER)
        j->mode = M_ENCODE;
    if (j->filter < 0 || j->filter >= K_COUNT)
        j->filter = K_LANCZOS3;
    if (j->rotate != 90 && j->rotate != 180 && j->rotate != 270)
        j->rotate = 0;
    if (j->quality > 100)
        j->quality = 100;
    if (j->compression > 9)
        j->compression = 9;
    if (j->colors > 256)
        j->colors = 256;
    if (!(j->max_pixels > 0))
        j->max_pixels = 268402689.0;
    if (JS_IsString(input)) {
        const char *s = JS_ToCString(ctx, input);
        if (!s)
            goto fail;
        j->path = strdup(s);
        JS_FreeCString(ctx, s);
        if (!j->path) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    } else {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, input, &len);
        if (!p)
            goto fail;
        j->input = malloc(len ? len : 1);
        if (!j->input) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        memcpy(j->input, p, len);
        j->input_len = len;
    }
    j->work.data = j;
    return j;
fail:
    job_free(j);
    return NULL;
}

static JSValue job_error(JSContext *ctx, QBImgJob *j)
{
    JSValue err;
    if (j->err.kind == E_SYS)
        return qb_uv_error(ctx, j->err.sys_err, j->err.syscall, j->path, NULL);
    err = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, j->err.msg, strlen(j->err.msg));
    if (JS_IsException(err))
        return JS_GetException(ctx);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, img_error_codes[j->err.kind]),
                              JS_PROP_C_W_E);
    return err;
}

/* fill args[1..4] (data, format, width, height); -1 with args[0] set to
   the error otherwise */
static int job_values(JSContext *ctx, QBImgJob *j, JSValue *args)
{
    args[1] = JS_UNDEFINED;
    if (j->mode != M_METADATA) {
        args[1] = qb_new_buffer(ctx, j->out ? j->out : (const uint8_t *)"", j->out_len);
        if (JS_IsException(args[1])) {
            args[0] = JS_GetException(ctx);
            args[1] = JS_UNDEFINED;
            return -1;
        }
    }
    args[2] = JS_NewString(ctx, img_format_names[j->out_format]);
    args[3] = JS_NewUint32(ctx, j->out_w);
    args[4] = JS_NewUint32(ctx, j->out_h);
    return 0;
}

static void img_work(uv_work_t *req)
{
    job_execute(req->data);
}

static void img_after(uv_work_t *req, int status)
{
    QBImgJob *j = req->data;
    JSContext *ctx = j->qr->ctx;
    JSValue args[5], ret;
    int i;
    for (i = 0; i < 5; i++)
        args[i] = JS_UNDEFINED;
    if (status < 0)
        args[0] = qb_uv_error(ctx, status, "image", NULL, NULL);
    else if (j->err.kind != E_NONE)
        args[0] = job_error(ctx, j);
    else if (!job_values(ctx, j, args))
        args[0] = JS_NULL;
    ret = qb_call(ctx, j->callback, JS_UNDEFINED, 5, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, j->callback);
    job_free(j);
}

/* run(input, options, callback) */
static JSValue qb_image_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBImgJob *j;
    int r;
    if (argc < 3 || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    j = job_new(ctx, argv[0], argv[1]);
    if (!j)
        return JS_EXCEPTION;
    j->callback = JS_DupValue(ctx, argv[2]);
    r = uv_queue_work(j->qr->loop, &j->work, img_work, img_after);
    if (r < 0) {
        JS_FreeValue(ctx, j->callback);
        job_free(j);
        return qb_throw_uv(ctx, r, "image", NULL, NULL);
    }
    return JS_UNDEFINED;
}

/* runSync(input, options) → { data, format, width, height } */
static JSValue qb_image_run_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QBImgJob *j;
    JSValue args[5], obj;
    int i;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "input and options are required");
    j = job_new(ctx, argv[0], argv[1]);
    if (!j)
        return JS_EXCEPTION;
    job_execute(j);
    for (i = 0; i < 5; i++)
        args[i] = JS_UNDEFINED;
    if (j->err.kind != E_NONE) {
        JSValue err = job_error(ctx, j);
        job_free(j);
        return JS_Throw(ctx, err);
    }
    if (job_values(ctx, j, args)) {
        job_free(j);
        return JS_Throw(ctx, args[0]);
    }
    job_free(j);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        for (i = 1; i < 5; i++)
            JS_FreeValue(ctx, args[i]);
        return obj;
    }
    /* qb_set() frees its value even on error: call it for each one */
    i = qb_set(ctx, obj, "data", args[1]);
    i |= qb_set(ctx, obj, "format", args[2]);
    i |= qb_set(ctx, obj, "width", args[3]);
    i |= qb_set(ctx, obj, "height", args[4]);
    if (i) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static const JSCFunctionListEntry qb_image_funcs[] = {
    JS_CFUNC_DEF("run", 3, qb_image_run),
    JS_CFUNC_DEF("runSync", 2, qb_image_run_sync),
};

QB_SIMPLE_MODULE(image, qb_image_funcs)

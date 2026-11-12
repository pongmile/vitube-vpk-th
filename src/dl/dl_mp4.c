/* ViTube — "is this a whole MP4?" for a finished download; see dl_core.h.
 *
 * Not a decoder and not a full validator. It answers the question that
 * matters before a .part is renamed to .mp4 and offered for playback: could
 * the bytes on the card be a file that ended where it should? A download that
 * was cut, a server that sent something else, a range written at the wrong
 * offset or a card that dropped a write all break the box structure, which is
 * cheap to walk: a handful of 8-byte headers read from the file, however big
 * the file is. */
#include "dl/dl_core.h"

#include <stdint.h>

/* A YouTube MP4 has fewer than ten top-level boxes. The cap only stops a
 * hostile or damaged file made of millions of 8-byte boxes from turning the
 * walk into millions of reads. */
#define MP4_BOX_LIMIT 4096

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const unsigned char *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* Top-level box types are four printable characters (ftyp, moov, mdat, free,
 * uuid, ...); 0xA9 is the (c) of the QuickTime metadata names. A box whose
 * "type" is anything else is not a box. This is what stops a zero-filled
 * tail — a hole left by a write that never happened — from passing as one
 * more box: a header of eight zero bytes reads as "size 0 = extends to the
 * end of the file". */
static int fourcc_ok(const unsigned char *t) {
    for (int i = 0; i < 4; i++)
        if (!(t[i] == 0xA9 || (t[i] >= 0x20 && t[i] < 0x7F))) return 0;
    return 1;
}

int dl_mp4_verify(DlReadFn read, void *ctx, int64_t size) {
    /* An ftyp box is at least 16 bytes: header, major brand, minor version. */
    if (size < 16) return DL_MP4_TOO_SMALL;
    int64_t pos = 0;
    int boxes = 0, moov = 0, mdat = 0;
    while (pos < size) {
        if (++boxes > MP4_BOX_LIMIT) return DL_MP4_TOO_MANY;
        /* Leftover bytes too few to be a header: trailing garbage. */
        if (size - pos < 8) return DL_MP4_TRUNCATED;
        unsigned char h[16];
        if (read(ctx, pos, h, 8) != 8) return DL_MP4_READ;
        const unsigned char *type = h + 4;
        if (boxes == 1 && !(type[0] == 'f' && type[1] == 't' && type[2] == 'y' &&
                            type[3] == 'p'))
            return DL_MP4_NO_FTYP;
        if (!fourcc_ok(type)) return DL_MP4_BAD_BOX;

        uint64_t box = be32(h);
        int64_t header = 8;
        if (box == 1) {
            /* 64-bit size follows the type. */
            if (size - pos < 16) return DL_MP4_TRUNCATED;
            if (read(ctx, pos + 8, h + 8, 8) != 8) return DL_MP4_READ;
            box = be64(h + 8);
            header = 16;
            if (box > (uint64_t)INT64_MAX) return DL_MP4_BAD_BOX;
        } else if (box == 0) {
            /* "Runs to the end of the file". */
            box = (uint64_t)(size - pos);
        }
        if (box < (uint64_t)header) return DL_MP4_BAD_BOX;
        if (box > (uint64_t)(size - pos)) return DL_MP4_TRUNCATED;
        if (boxes == 1 && box < 16) return DL_MP4_BAD_BOX;

        if (type[0] == 'm' && type[1] == 'o' && type[2] == 'o' && type[3] == 'v')
            moov = 1;
        else if (type[0] == 'm' && type[1] == 'd' && type[2] == 'a' &&
                 type[3] == 't')
            mdat = 1;
        pos += (int64_t)box;
    }
    if (!moov) return DL_MP4_NO_MOOV;
    if (!mdat) return DL_MP4_NO_MDAT;
    return DL_MP4_OK;
}

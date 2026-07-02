/* dbf.h — Deshab Bitmap Font format reader
 * 
 * DBF File Layout:
 *   [0..3]   magic      "DBF\x10" (version 16)
 *   [4..7]   u32        glyph_count
 *   [8..9]   u16        default_glyph_width
 *   [10..11] u16        default_glyph_height
 *   [12..13] u16        bits_per_pixel (8 = grayscale, 1 = BW)
 *   [14..]   index[]    array of glyph_count entries:
 *              [0..1]   u16  codepoint (Unicode BMP)
 *              [2..3]   u16  actual_width (pixels)
 *              [4..7]   u32  data_offset (from file start)
 *              [8..11]  u32  data_size (bytes)
 *            Each index entry is 12 bytes.
 *   [14 + index*12 ..]  glyph_bitmap[] raw 8bpp row-major data
 *
 * Usage:
 *   const u8 *font_data = ...;  // mmap or load entire .dbf
 *   dbf_header *hdr = (dbf_header *)font_data;
 *   dbf_index_entry *idx = (dbf_index_entry *)(font_data + 14);
 *   // binary search for codepoint in idx[0..hdr->count-1]
 *   // glyph data at font_data + idx[i].offset, size idx[i].size
 */

#ifndef DBF_H
#define DBF_H

#include <stdint.h>

typedef struct {
    uint8_t  magic[4];   /* "DBF\x10" */
    uint32_t count;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
} __attribute__((packed)) dbf_header;

typedef struct {
    uint16_t codepoint;
    uint16_t width;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) dbf_index_entry;

/* Binary search for codepoint. Returns entry index or -1. */
static inline int dbf_lookup(dbf_index_entry *idx, uint32_t count, uint16_t cp) {
    int lo = 0, hi = (int)count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (idx[mid].codepoint == cp) return mid;
        if (idx[mid].codepoint < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

#endif

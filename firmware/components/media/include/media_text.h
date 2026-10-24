/*
 * The text source: UTF-8 strings rendered into I420 frames with the bitmap
 * font that tools/make_font.py compiles into font16.bin (embedded in the
 * firmware image on the chip, loaded from the tree by host tests).
 *
 * Every glyph lives in a fixed-size cell and advances the pen by the cell
 * width, so Latin and Chinese share one monospace grid. A line that fits the
 * picture sits centred and static; a wider line scrolls right-to-left and
 * wraps around.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest accepted UTF-8 string, in bytes (about 40 hanzi). */
#define MEDIA_TEXT_MAX_BYTES 128

/* Installs a compiled font image (the tools/make_font.py format). The
 * firmware maps the flash-resident font16.bin by itself; host tests use this
 * to hand over the file's contents. Returns false when the image is
 * malformed, leaving any previous font in place. */
bool media_text_load_font(const uint8_t *data, size_t len);

/* Replaces the on-screen text. An empty string (or NULL) clears it, and the
 * live source goes back to the test pattern. Returns false when the string
 * does not fit MEDIA_TEXT_MAX_BYTES; the previous text stays. */
bool media_text_set(const char *utf8);

/* Copies the current text (empty when none is set). */
void media_text_get(char *buf, size_t len);

/* Renders the current text into yuv (I420, the test pattern's geometry: the
 * encoder consumes both interchangeably). Returns false when no text is set,
 * no font is available, or the text contains no glyph at all - the caller
 * renders the test pattern instead. Safe to call before any font exists. */
bool media_text_generate(uint8_t *yuv, size_t len, uint32_t step);

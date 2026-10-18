#pragma once

/* A small C wrapper around MuPDF, so the C++ side never sees fz_try (it is
 * setjmp/longjmp: C++ objects with destructors must not live inside it).
 *
 * One cr_ctx per thread. A book belongs to the context that opened it and is
 * only used on that thread. Errors come back as NULL / -1 plus a message. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cr_ctx cr_ctx;
typedef struct cr_book cr_book;

cr_ctx *cr_ctx_new(void);
void cr_ctx_drop(cr_ctx *ctx);

/* A book: PDF, CBZ, CBR, CB7, CBT, a single image, or a folder of images. */
cr_book *cr_open(cr_ctx *ctx, const char *path, char *err, int errlen);
void cr_close(cr_book *book);
int cr_page_count(cr_book *book);

/* Page `page` (0-based) drawn as large as fits in maxW x maxH, aspect kept, on
 * white: RGBA, 4 bytes a pixel, rows packed. malloc'd: the caller free()s it.
 * NULL on error. */
unsigned char *cr_render(cr_book *book, int page, int maxW, int maxH, int *w, int *h, char *err, int errlen);

/* Saves RGBA pixels as a JPEG (alpha dropped). 0 on success. */
int cr_save_jpeg(cr_ctx *ctx, const unsigned char *rgba, int w, int h, const char *path, int quality);

/* Decodes an image file to RGBA (malloc'd), NULL on error. */
unsigned char *cr_load_image(cr_ctx *ctx, const char *path, int *w, int *h);

/* The text of ComicInfo.xml inside a CBZ/CBR/CB7/CBT, malloc'd, or NULL. */
char *cr_comicinfo(cr_ctx *ctx, const char *path);

/* True if `name` looks like a page image (by extension). */
int cr_is_image_name(const char *name);

/* Natural order: "page2" before "page10", case-insensitive. */
int cr_natcmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

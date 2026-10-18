/* MuPDF behind a plain C interface; see Doc.h. */
#include "Doc.h"

#include <mupdf/fitz.h>
#include <webp/decode.h>

#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct cr_ctx {
	fz_context *fz;
};

struct cr_book {
	cr_ctx *ctx;
	fz_document *doc;  /* a document, or NULL for a folder of images */
	char **files;      /* the folder's images, in reading order */
	int nfiles;
};

static void set_err(char *err, int errlen, const char *msg)
{
	if (err && errlen > 0) {
		strncpy(err, msg ? msg : "unknown error", (size_t)errlen - 1);
		err[errlen - 1] = 0;
	}
}

static void quiet(void *user, const char *message)
{
	(void)user;
	(void)message;
}

/* MuPDF across threads: one base context with real locks, and a clone of it
 * per thread. Contexts made separately with fz_new_context and no locks are
 * NOT independent: MuPDF keeps some state in globals (OpenJPEG's allocator
 * context, FreeType) and guards it with these locks. Two threads decoding a
 * JPEG 2000 page at once crashed the app on the Manuals shelf. */
static pthread_mutex_t g_mutex[FZ_LOCK_MAX];
static fz_context *g_base;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void lock(void *user, int n)
{
	(void)user;
	pthread_mutex_lock(&g_mutex[n]);
}

static void unlock(void *user, int n)
{
	(void)user;
	pthread_mutex_unlock(&g_mutex[n]);
}

static void make_base(void)
{
	static fz_locks_context locks;
	int i;
	for (i = 0; i < FZ_LOCK_MAX; i++)
		pthread_mutex_init(&g_mutex[i], NULL);
	locks.user = NULL;
	locks.lock = lock;
	locks.unlock = unlock;
	/* Up to 256 MB of decoded images, shared by every thread. */
	g_base = fz_new_context(NULL, &locks, 256u << 20);
	if (!g_base)
		return;
	fz_set_warning_callback(g_base, quiet, NULL);
	fz_try(g_base)
		fz_register_document_handlers(g_base);
	fz_catch(g_base)
	{
		fz_drop_context(g_base);
		g_base = NULL;
	}
}

cr_ctx *cr_ctx_new(void)
{
	cr_ctx *c;
	pthread_once(&g_once, make_base);
	if (!g_base)
		return NULL;
	c = (cr_ctx *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->fz = fz_clone_context(g_base);
	if (!c->fz) {
		free(c);
		return NULL;
	}
	fz_set_warning_callback(c->fz, quiet, NULL);
	fz_set_error_callback(c->fz, quiet, NULL);
	return c;
}

void cr_ctx_drop(cr_ctx *ctx)
{
	if (!ctx)
		return;
	fz_drop_context(ctx->fz);
	free(ctx);
}

/* ------------------------------------------------------------ WebP */

/* MuPDF has no WebP decoder; its image loader calls this (our patch,
 * tools/patches/mupdf-webp.patch) for RIFF/WEBP data. libwebp gives straight
 * RGBA; MuPDF's pixmaps with alpha are premultiplied. */
fz_pixmap *cr_decode_webp(fz_context *ctx, const unsigned char *data, size_t len)
{
	int w = 0, h = 0, x, y;
	uint8_t *rgba;
	fz_pixmap *pix;
	unsigned char *dst;
	ptrdiff_t stride;
	if (!WebPGetInfo(data, len, &w, &h) || w <= 0 || h <= 0)
		fz_throw(ctx, FZ_ERROR_FORMAT, "not a WebP picture");
	rgba = WebPDecodeRGBA(data, len, &w, &h);
	if (!rgba)
		fz_throw(ctx, FZ_ERROR_FORMAT, "can't decode this WebP picture");
	fz_try(ctx)
		pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, NULL, 1);
	fz_catch(ctx)
	{
		WebPFree(rgba);
		fz_rethrow(ctx);
	}
	dst = fz_pixmap_samples(ctx, pix);
	stride = fz_pixmap_stride(ctx, pix);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			const uint8_t *s = rgba + ((size_t)y * w + x) * 4;
			unsigned char *d = dst + y * stride + x * 4;
			const unsigned a = s[3];
			d[0] = (unsigned char)((s[0] * a + 127) / 255);
			d[1] = (unsigned char)((s[1] * a + 127) / 255);
			d[2] = (unsigned char)((s[2] * a + 127) / 255);
			d[3] = (unsigned char)a;
		}
	WebPFree(rgba);
	return pix;
}

/* ------------------------------------------------------------ names */

int cr_is_image_name(const char *name)
{
	static const char *exts[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff", ".jp2", ".jpx", ".webp", NULL};
	const char *dot = strrchr(name, '.');
	int i;
	if (!dot)
		return 0;
	for (i = 0; exts[i]; i++)
		if (!fz_strcasecmp(dot, exts[i]))
			return 1;
	return 0;
}

int cr_natcmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			const char *sa, *sb;
			size_t la, lb;
			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			sa = a;
			sb = b;
			while (isdigit((unsigned char)*a))
				a++;
			while (isdigit((unsigned char)*b))
				b++;
			la = (size_t)(a - sa);
			lb = (size_t)(b - sb);
			if (la != lb)
				return la < lb ? -1 : 1;
			{
				int c = memcmp(sa, sb, la);
				if (c)
					return c;
			}
		} else {
			int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
			if (ca != cb)
				return ca < cb ? -1 : 1;
			a++;
			b++;
		}
	}
	return *a ? 1 : *b ? -1 : 0;
}

static int natcmp_ptr(const void *a, const void *b)
{
	return cr_natcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------------------------------------------ books */

static int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int list_images(const char *dir, char ***out)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char **files = NULL;
	int n = 0, cap = 0;
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		size_t len;
		char *full;
		if (e->d_name[0] == '.' || !cr_is_image_name(e->d_name))
			continue;
		if (n == cap) {
			char **more;
			cap = cap ? cap * 2 : 64;
			more = (char **)realloc(files, (size_t)cap * sizeof(*files));
			if (!more)
				break;
			files = more;
		}
		len = strlen(dir) + strlen(e->d_name) + 2;
		full = (char *)malloc(len);
		if (!full)
			break;
		strcpy(full, dir);
		strcat(full, "/");
		strcat(full, e->d_name);
		files[n++] = full;
	}
	closedir(d);
	if (n > 1)
		qsort(files, (size_t)n, sizeof(*files), natcmp_ptr);
	*out = files;
	return n;
}

cr_book *cr_open(cr_ctx *ctx, const char *path, char *err, int errlen)
{
	cr_book *book = (cr_book *)calloc(1, sizeof(*book));
	if (!book) {
		set_err(err, errlen, "out of memory");
		return NULL;
	}
	book->ctx = ctx;
	if (is_dir(path)) {
		book->nfiles = list_images(path, &book->files);
		if (book->nfiles <= 0) {
			set_err(err, errlen, "no pictures in this folder");
			cr_close(book);
			return NULL;
		}
		return book;
	}
	fz_try(ctx->fz)
		book->doc = fz_open_document(ctx->fz, path);
	fz_catch(ctx->fz)
	{
		set_err(err, errlen, fz_caught_message(ctx->fz));
		free(book);
		return NULL;
	}
	return book;
}

void cr_close(cr_book *book)
{
	int i;
	if (!book)
		return;
	if (book->doc)
		fz_drop_document(book->ctx->fz, book->doc);
	for (i = 0; i < book->nfiles; i++)
		free(book->files[i]);
	free(book->files);
	free(book);
}

int cr_page_count(cr_book *book)
{
	int n = -1;
	if (!book)
		return -1;
	if (!book->doc)
		return book->nfiles;
	fz_try(book->ctx->fz)
		n = fz_count_pages(book->ctx->fz, book->doc);
	fz_catch(book->ctx->fz)
		n = -1;
	return n;
}

static unsigned char *render(cr_book *book, int page, int maxW, int maxH, float extra, const fz_irect *clip,
                             int *ox, int *oy, int *w, int *h, char *err, int errlen)
{
	fz_context *fz = book->ctx->fz;
	fz_document *volatile single = NULL;
	fz_page *volatile pg = NULL;
	fz_pixmap *volatile pix = NULL;
	fz_device *volatile dev = NULL;
	unsigned char *volatile out = NULL;

	fz_try(fz)
	{
		fz_document *doc = book->doc;
		int index = page;
		fz_rect b;
		float pw, ph, zoom;
		fz_matrix ctm;
		fz_irect ib;
		int y, W, H;
		const unsigned char *src;
		ptrdiff_t stride;

		if (!doc) {
			if (page < 0 || page >= book->nfiles)
				fz_throw(fz, FZ_ERROR_ARGUMENT, "no page %d", page + 1);
			single = fz_open_document(fz, book->files[page]);
			doc = single;
			index = 0;
		}
		pg = fz_load_page(fz, doc, index);
		b = fz_bound_page(fz, pg);
		pw = b.x1 - b.x0;
		ph = b.y1 - b.y0;
		if (pw <= 0 || ph <= 0)
			fz_throw(fz, FZ_ERROR_FORMAT, "empty page");
		zoom = fz_min(maxW / pw, maxH / ph) * extra;
		/* Page origin at (0, 0), so a clip is in the scaled page's own pixels. */
		ctm = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(zoom, zoom));
		ib = fz_round_rect(fz_transform_rect(b, ctm));
		if (clip)
			ib = fz_intersect_irect(ib, *clip);
		if (fz_is_empty_irect(ib))
			fz_throw(fz, FZ_ERROR_ARGUMENT, "nothing to draw there");
		pix = fz_new_pixmap_with_bbox(fz, fz_device_rgb(fz), ib, NULL, 1);
		fz_clear_pixmap_with_value(fz, pix, 255); /* opaque white */
		dev = fz_new_draw_device(fz, ctm, pix);
		fz_run_page(fz, pg, dev, fz_identity, NULL);
		fz_close_device(fz, dev);

		W = fz_pixmap_width(fz, pix);
		H = fz_pixmap_height(fz, pix);
		src = fz_pixmap_samples(fz, pix);
		stride = fz_pixmap_stride(fz, pix);
		out = (unsigned char *)malloc((size_t)W * H * 4);
		if (!out)
			fz_throw(fz, FZ_ERROR_SYSTEM, "out of memory");
		for (y = 0; y < H; y++)
			memcpy(out + (size_t)y * W * 4, src + y * stride, (size_t)W * 4);
		*w = W;
		*h = H;
		if (ox)
			*ox = ib.x0;
		if (oy)
			*oy = ib.y0;
	}
	fz_always(fz)
	{
		fz_drop_device(fz, dev);
		fz_drop_pixmap(fz, pix);
		fz_drop_page(fz, pg);
		fz_drop_document(fz, single);
	}
	fz_catch(fz)
	{
		set_err(err, errlen, fz_caught_message(fz));
		free(out);
		return NULL;
	}
	return out;
}

unsigned char *cr_render(cr_book *book, int page, int maxW, int maxH, int *w, int *h, char *err, int errlen)
{
	return render(book, page, maxW, maxH, 1.0f, NULL, NULL, NULL, w, h, err, errlen);
}

unsigned char *cr_render_region(cr_book *book, int page, int fitW, int fitH, float zoom, int x, int y, int w, int h,
                                int *outX, int *outY, int *outW, int *outH, char *err, int errlen)
{
	fz_irect clip;
	clip.x0 = x;
	clip.y0 = y;
	clip.x1 = x + w;
	clip.y1 = y + h;
	return render(book, page, fitW, fitH, zoom, &clip, outX, outY, outW, outH, err, errlen);
}

int cr_save_jpeg(cr_ctx *ctx, const unsigned char *rgba, int w, int h, const char *path, int quality)
{
	fz_context *fz = ctx->fz;
	fz_pixmap *volatile pix = NULL;
	int ok = -1;
	fz_try(fz)
	{
		unsigned char *dst;
		size_t i, n = (size_t)w * h;
		pix = fz_new_pixmap(fz, fz_device_rgb(fz), w, h, NULL, 0);
		dst = fz_pixmap_samples(fz, pix);
		/* A fresh pixmap's rows are packed (stride w*3). */
		for (i = 0; i < n; i++) {
			dst[i * 3] = rgba[i * 4];
			dst[i * 3 + 1] = rgba[i * 4 + 1];
			dst[i * 3 + 2] = rgba[i * 4 + 2];
		}
		fz_save_pixmap_as_jpeg(fz, pix, path, quality);
		ok = 0;
	}
	fz_always(fz)
		fz_drop_pixmap(fz, pix);
	fz_catch(fz)
		ok = -1;
	return ok;
}

unsigned char *cr_load_image(cr_ctx *ctx, const char *path, int *w, int *h)
{
	fz_context *fz = ctx->fz;
	fz_image *volatile img = NULL;
	fz_pixmap *volatile pix = NULL;
	fz_pixmap *volatile rgb = NULL;
	unsigned char *volatile out = NULL;
	fz_try(fz)
	{
		int W, H, y, x, n;
		const unsigned char *src;
		ptrdiff_t stride;
		img = fz_new_image_from_file(fz, path);
		pix = fz_get_pixmap_from_image(fz, img, NULL, NULL, NULL, NULL);
		rgb = fz_convert_pixmap(fz, pix, fz_device_rgb(fz), NULL, NULL, fz_default_color_params, 1);
		W = fz_pixmap_width(fz, rgb);
		H = fz_pixmap_height(fz, rgb);
		n = fz_pixmap_components(fz, rgb);
		src = fz_pixmap_samples(fz, rgb);
		stride = fz_pixmap_stride(fz, rgb);
		out = (unsigned char *)malloc((size_t)W * H * 4);
		if (!out)
			fz_throw(fz, FZ_ERROR_SYSTEM, "out of memory");
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				const unsigned char *s = src + y * stride + x * n;
				unsigned char *d = out + ((size_t)y * W + x) * 4;
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = n > 3 ? s[3] : 255;
			}
		*w = W;
		*h = H;
	}
	fz_always(fz)
	{
		fz_drop_pixmap(fz, rgb);
		fz_drop_pixmap(fz, pix);
		fz_drop_image(fz, img);
	}
	fz_catch(fz)
	{
		free(out);
		return NULL;
	}
	return out;
}

char *cr_comicinfo(cr_ctx *ctx, const char *path)
{
	fz_context *fz = ctx->fz;
	fz_archive *volatile arch = NULL;
	fz_buffer *volatile buf = NULL;
	char *volatile out = NULL;
	if (is_dir(path))
		return NULL;
	fz_try(fz)
	{
		int i, n;
		const char *found = NULL;
		arch = fz_open_archive(fz, path);
		n = fz_count_archive_entries(fz, arch);
		for (i = 0; i < n && !found; i++) {
			const char *name = fz_list_archive_entry(fz, arch, i);
			const char *base = name ? strrchr(name, '/') : NULL;
			base = base ? base + 1 : name;
			if (base && !fz_strcasecmp(base, "ComicInfo.xml") && !strstr(name, "__MACOSX/"))
				found = name;
		}
		if (found) {
			unsigned char *data;
			size_t len;
			buf = fz_read_archive_entry(fz, arch, found);
			len = fz_buffer_storage(fz, buf, &data);
			if (len < (4u << 20)) {
				out = (char *)malloc(len + 1);
				if (out) {
					memcpy(out, data, len);
					out[len] = 0;
				}
			}
		}
	}
	fz_always(fz)
	{
		fz_drop_buffer(fz, buf);
		fz_drop_archive(fz, arch);
	}
	fz_catch(fz)
	{
		free(out);
		return NULL;
	}
	return out;
}

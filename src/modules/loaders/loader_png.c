#include "loader_common.h"

#include <png.h>
#include <stdint.h>
#include <sys/mman.h>

#define DBG_PFX "LDR-png"

#define USE_IMLIB2_COMMENT_TAG 0

#define _PNG_MIN_SIZE	60      /* Min. PNG file size (8 + 3*12 + 13 (+3) */
#define _PNG_SIG_SIZE	8       /* Signature size */

typedef struct {
   ImlibImage         *im;
   char                load_data;
   char                rc;

   char                interlace;
} ctx_t;

#if 0
static const unsigned char png_sig[] = {
   0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};
#endif

#if USE_IMLIB2_COMMENT_TAG
static void
comment_free(ImlibImage * im, void *data)
{
   free(data);
}
#endif

static void
user_error_fn(png_struct * png_ptr, const char *txt)
{
#if 0
   D("%s: %s\n", __func__, txt);
#endif
}

static void
user_warning_fn(png_struct * png_ptr, const char *txt)
{
   D("%s: %s\n", __func__, txt);
}

static void
info_callback(png_struct * png_ptr, png_info * info_ptr)
{
   int                 rc;
   ctx_t              *ctx = png_get_progressive_ptr(png_ptr);
   ImlibImage         *im = ctx->im;
   png_uint_32         w32, h32;
   int                 bit_depth, color_type, interlace_type;
   int                 hasa;

   rc = LOAD_BADIMAGE;          /* Format accepted */

   png_get_IHDR(png_ptr, info_ptr, &w32, &h32, &bit_depth, &color_type,
                &interlace_type, NULL, NULL);

   D("%s: WxH=%dx%d depth=%d color=%d interlace=%d\n", __func__,
     w32, h32, bit_depth, color_type, interlace_type);

   im->w = w32;
   im->h = h32;

   if (!IMAGE_DIMENSIONS_OK(w32, h32))
      goto quit;

   hasa = 0;
   if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
      hasa = 1;
   if (color_type == PNG_COLOR_TYPE_RGB_ALPHA)
      hasa = 1;
   if (color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
      hasa = 1;
   UPDATE_FLAG(im->flags, F_HAS_ALPHA, hasa);

   if (!ctx->load_data)
      QUIT_WITH_RC(LOAD_SUCCESS);

   /* Load data */

   /* Prep for transformations...  ultimately we want ARGB */
   /* expand palette -> RGB if necessary */
   if (color_type == PNG_COLOR_TYPE_PALETTE)
      png_set_palette_to_rgb(png_ptr);
   /* expand gray (w/reduced bits) -> 8-bit RGB if necessary */
   if ((color_type == PNG_COLOR_TYPE_GRAY) ||
       (color_type == PNG_COLOR_TYPE_GRAY_ALPHA))
     {
        png_set_gray_to_rgb(png_ptr);
        if (bit_depth < 8)
           png_set_expand_gray_1_2_4_to_8(png_ptr);
     }
   /* expand transparency entry -> alpha channel if present */
   if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
      png_set_tRNS_to_alpha(png_ptr);
   /* reduce 16bit color -> 8bit color if necessary */
   if (bit_depth > 8)
      png_set_strip_16(png_ptr);
   /* pack all pixels to byte boundaries */
   png_set_packing(png_ptr);

/* note from raster:                                                         */
/* thanks to mustapha for helping debug this on PPC Linux remotely by        */
/* sending across screenshots all the time and me figuring out from them     */
/* what the hell was up with the colors                                      */
/* now png loading should work on big-endian machines nicely                 */
#ifdef WORDS_BIGENDIAN
   png_set_swap_alpha(png_ptr);
   if (!hasa)
      png_set_filler(png_ptr, 0xff, PNG_FILLER_BEFORE);
#else
   png_set_bgr(png_ptr);
   if (!hasa)
      png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
#endif

   /* NB! If png_read_update_info() isn't called processing stops here */
   png_read_update_info(png_ptr, info_ptr);

   if (!__imlib_AllocateData(im))
      QUIT_WITH_RC(LOAD_OOM);

   rc = LOAD_SUCCESS;

 quit:
   ctx->rc = rc;
   if (!ctx->load_data || rc != LOAD_SUCCESS)
      png_longjmp(png_ptr, 1);
}

static void
row_callback(png_struct * png_ptr, png_byte * new_row,
             png_uint_32 row_num, int pass)
{
   ctx_t              *ctx = png_get_progressive_ptr(png_ptr);
   ImlibImage         *im = ctx->im;
   DATA32             *dptr;
   const DATA32       *sptr;
   int                 x, y, x0, dx, y0, dy;
   int                 done;

   DL("%s: png=%p data=%p row=%d, pass=%d\n", __func__, png_ptr, new_row,
      row_num, pass);

   if (!im->data)
      return;

   if (ctx->interlace)
     {
        x0 = PNG_PASS_START_COL(pass);
        dx = PNG_PASS_COL_OFFSET(pass);
        y0 = PNG_PASS_START_ROW(pass);
        dy = PNG_PASS_ROW_OFFSET(pass);

        DL("x0, dx = %d,%d y0,dy = %d,%d cols/rows=%d/%d\n",
           x0, dx, y0, dy,
           PNG_PASS_COLS(im->w, pass), PNG_PASS_ROWS(im->h, pass));
        y = y0 + dy * row_num;

        sptr = (const DATA32 *)new_row; /* Assuming aligned */
        dptr = im->data + y * im->w;
        for (x = x0; x < im->w; x += dx)
          {
#if 0
             dptr[x] = PIXEL_ARGB(new_row[ii + 3], new_row[ii + 2],
                                  new_row[ii + 1], new_row[ii + 0]);
             D("x,y = %d,%d (i,j, ii=%d,%d %d): %08x\n", x, y,
               ii / 4, row_num, ii, dptr[x]);
#else
             dptr[x] = *sptr++;
#endif
          }

        done = pass >= 6 && (int)row_num >= PNG_PASS_ROWS(im->h, pass) - 1;
        if (im->lc && done)
           __imlib_LoadProgress(im, im->frame_x, im->frame_y, im->w, im->h);
     }
   else
     {
        y = row_num;

        dptr = im->data + y * im->w;
        memcpy(dptr, new_row, sizeof(DATA32) * im->w);

        done = (int)row_num >= im->h - 1;

        if (im->lc)
          {
             if (im->frame_count > 1)
               {
                  if (done)
                     __imlib_LoadProgress(im, im->frame_x, im->frame_y, im->w,
                                          im->h);
               }
             else if (__imlib_LoadProgressRows(im, y, 1))
               {
                  png_process_data_pause(png_ptr, 0);
                  ctx->rc = LOAD_BREAK;
               }
          }
     }
}

int
load2(ImlibImage * im, int load_data)
{
   int                 rc;
   void               *fdata;
   png_structp         png_ptr = NULL;
   png_infop           info_ptr = NULL;
   ctx_t               ctx = { 0 };

   /* read header */
   rc = LOAD_FAIL;

   if (im->fsize < _PNG_MIN_SIZE)
      return rc;

   fdata = mmap(NULL, im->fsize, PROT_READ, MAP_SHARED, fileno(im->fp), 0);
   if (fdata == MAP_FAILED)
      return LOAD_BADFILE;

   /* Signature check */
   if (png_sig_cmp(fdata, 0, _PNG_SIG_SIZE))
      goto quit;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                    user_error_fn, user_warning_fn);
   if (!png_ptr)
      goto quit;

   info_ptr = png_create_info_struct(png_ptr);
   if (!info_ptr)
      goto quit;

   rc = LOAD_BADIMAGE;          /* Format accepted */

   ctx.im = im;
   ctx.load_data = load_data;
   ctx.rc = rc;

   if (setjmp(png_jmpbuf(png_ptr)))
      QUIT_WITH_RC(ctx.rc);

   /* Set up processing via callbacks */
   png_set_progressive_read_fn(png_ptr, &ctx,
                               info_callback, row_callback, NULL);

   png_process_data(png_ptr, info_ptr, fdata, im->fsize);

   rc = ctx.rc;
   if (rc <= 0)
      goto quit;

   rc = LOAD_SUCCESS;

#if USE_IMLIB2_COMMENT_TAG
#ifdef PNG_TEXT_SUPPORTED
   {
      png_textp           text;
      int                 i, num;

      num = 0;
      png_get_text(png_ptr, info_ptr, &text, &num);
      for (i = 0; i < num; i++)
        {
           if (!strcmp(text[i].key, "Imlib2-Comment"))
              __imlib_AttachTag(im, "comment", 0, strdup(text[i].text),
                                comment_free);
        }
   }
#endif
#endif

 quit:
   png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   if (rc <= 0)
      __imlib_FreeData(im);
   munmap(fdata, im->fsize);

   return rc;
}

char
save(ImlibImage * im, ImlibProgressFunction progress, char progress_granularity)
{
   int                 rc;
   FILE               *f;
   png_structp         png_ptr;
   png_infop           info_ptr;
   DATA32             *ptr;
   int                 x, y, j, interlace;
   png_bytep           row_ptr, data;
   png_color_8         sig_bit;
   ImlibImageTag      *tag;
   int                 quality = 75, compression = 3;
   int                 pass, n_passes = 1;
   int                 has_alpha;

   f = fopen(im->real_file, "wb");
   if (!f)
      return LOAD_FAIL;

   rc = LOAD_FAIL;
   info_ptr = NULL;
   data = NULL;

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (!png_ptr)
      goto quit;

   info_ptr = png_create_info_struct(png_ptr);
   if (!info_ptr)
      goto quit;

   if (setjmp(png_jmpbuf(png_ptr)))
      goto quit;

   /* check whether we should use interlacing */
   interlace = PNG_INTERLACE_NONE;
   if ((tag = __imlib_GetTag(im, "interlacing")) && tag->val)
     {
#ifdef PNG_WRITE_INTERLACING_SUPPORTED
        interlace = PNG_INTERLACE_ADAM7;
#endif
     }

   png_init_io(png_ptr, f);
   has_alpha = IMAGE_HAS_ALPHA(im);
   if (has_alpha)
     {
        png_set_IHDR(png_ptr, info_ptr, im->w, im->h, 8,
                     PNG_COLOR_TYPE_RGB_ALPHA, interlace,
                     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
#ifdef WORDS_BIGENDIAN
        png_set_swap_alpha(png_ptr);
#else
        png_set_bgr(png_ptr);
#endif
     }
   else
     {
        png_set_IHDR(png_ptr, info_ptr, im->w, im->h, 8, PNG_COLOR_TYPE_RGB,
                     interlace, PNG_COMPRESSION_TYPE_BASE,
                     PNG_FILTER_TYPE_BASE);
        data = malloc(im->w * 3 * sizeof(png_byte));
     }
   sig_bit.red = 8;
   sig_bit.green = 8;
   sig_bit.blue = 8;
   sig_bit.alpha = 8;
   png_set_sBIT(png_ptr, info_ptr, &sig_bit);

   /* quality */
   tag = __imlib_GetTag(im, "quality");
   if (tag)
     {
        quality = tag->val;
        if (quality < 1)
           quality = 1;
        if (quality > 99)
           quality = 99;
     }
   /* convert to compression */
   quality = quality / 10;
   compression = 9 - quality;
   /* compression */
   tag = __imlib_GetTag(im, "compression");
   if (tag)
      compression = tag->val;
   if (compression < 0)
      compression = 0;
   if (compression > 9)
      compression = 9;
   png_set_compression_level(png_ptr, compression);

#if USE_IMLIB2_COMMENT_TAG
   tag = __imlib_GetTag(im, "comment");
   if (tag)
     {
#ifdef PNG_TEXT_SUPPORTED
        png_text            text;

        text.key = (char *)"Imlib2-Comment";
        text.text = tag->data;
        text.compression = PNG_TEXT_COMPRESSION_zTXt;
        png_set_text(png_ptr, info_ptr, &(text), 1);
#endif
     }
#endif

   png_write_info(png_ptr, info_ptr);
   png_set_shift(png_ptr, &sig_bit);
   png_set_packing(png_ptr);

#ifdef PNG_WRITE_INTERLACING_SUPPORTED
   n_passes = png_set_interlace_handling(png_ptr);
#endif

   for (pass = 0; pass < n_passes; pass++)
     {
        ptr = im->data;

        if (im->lc)
           __imlib_LoadProgressSetPass(im, pass, n_passes);

        for (y = 0; y < im->h; y++)
          {
             if (has_alpha)
                row_ptr = (png_bytep) ptr;
             else
               {
                  for (j = 0, x = 0; x < im->w; x++)
                    {
                       DATA32              pixel = ptr[x];

                       data[j++] = PIXEL_R(pixel);
                       data[j++] = PIXEL_G(pixel);
                       data[j++] = PIXEL_B(pixel);
                    }
                  row_ptr = (png_bytep) data;
               }
             png_write_rows(png_ptr, &row_ptr, 1);

             if (im->lc && __imlib_LoadProgressRows(im, y, 1))
                QUIT_WITH_RC(LOAD_BREAK);

             ptr += im->w;
          }
     }

   rc = LOAD_SUCCESS;

 quit:
   free(data);
   png_write_end(png_ptr, info_ptr);
   png_destroy_write_struct(&png_ptr, (png_infopp) & info_ptr);
   if (info_ptr)
      png_destroy_info_struct(png_ptr, (png_infopp) & info_ptr);
   if (png_ptr)
      png_destroy_write_struct(&png_ptr, (png_infopp) NULL);

   fclose(f);

   return rc;
}

void
formats(ImlibLoader * l)
{
   static const char  *const list_formats[] = { "png" };
   __imlib_LoaderSetFormats(l, list_formats, ARRAY_SIZE(list_formats));
}

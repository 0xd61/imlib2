#include "loader_common.h"

#include <math.h>
#include <sys/mman.h>
#include <librsvg/rsvg.h>

#define DBG_PFX "LDR-svg"

#define DPI 96

static double
u2pix(double x, int unit)
{
   switch (unit)
     {
     default:
     case RSVG_UNIT_PERCENT:   /* 0  percentage values where 1.0 means 100% */
     case RSVG_UNIT_PX:        /* 1  pixels */
     case RSVG_UNIT_EM:        /* 2  em, or the current font size */
     case RSVG_UNIT_EX:        /* 3  x-height of the current font */
        return x;
     case RSVG_UNIT_IN:        /* 4  inches */
        return x * DPI;
     case RSVG_UNIT_CM:        /* 5  centimeters */
        return x * DPI / 2.54;
     case RSVG_UNIT_MM:        /* 6  millimeters */
        return x * DPI / 25.4;
     case RSVG_UNIT_PT:        /* 7  points, or 1/72 inch */
        return x * DPI / 72;
     case RSVG_UNIT_PC:        /* 8  picas, or 1/6 inch (12 points) */
        return x * DPI / 6;
     }
}

int
load2(ImlibImage * im, int load_data)
{
   int                 rc;
   void               *fdata;
   RsvgHandle         *rsvg;
   GError             *error;
   gboolean            ok;
   cairo_surface_t    *surface;
   cairo_t            *cr;
   RsvgRectangle       cvb;

   rc = LOAD_FAIL;

   fdata = mmap(NULL, im->fsize, PROT_READ, MAP_SHARED, fileno(im->fp), 0);
   if (fdata == MAP_FAILED)
      return LOAD_BADFILE;

   surface = NULL;
   cr = NULL;

   error = NULL;
   rsvg = rsvg_handle_new_from_data(fdata, im->fsize, &error);
   if (!rsvg)
      goto quit;

   rc = LOAD_BADIMAGE;          /* Format accepted */

   {
      gboolean            out_has_width, out_has_height, out_has_viewbox;
      RsvgLength          out_width = { }, out_height = { };
      RsvgRectangle       out_viewbox = { };
      rsvg_handle_get_intrinsic_dimensions(rsvg,
                                           &out_has_width,
                                           &out_width,
                                           &out_has_height,
                                           &out_height,
                                           &out_has_viewbox, &out_viewbox);
      D("WH:%d%d %.1fx%.1f (%d/%d: %.1fx%.1f) VB:%d %.1f,%.1f %.1fx%.1f\n",
        out_has_width, out_has_height,
        out_width.length, out_height.length, out_width.unit, out_height.unit,
        u2pix(out_width.length, out_width.unit),
        u2pix(out_height.length, out_height.unit),
        out_has_viewbox,
        out_viewbox.x, out_viewbox.y, out_viewbox.width, out_viewbox.height);

      if (out_has_width && out_has_height)
        {
           im->w = lrint(u2pix(out_width.length, out_width.unit));
           im->h = lrint(u2pix(out_height.length, out_height.unit));
           D("Choose rsvg_handle_get_intrinsic_dimensions width/height\n");
#if !IMLIB2_DEBUG
           goto got_size;
#endif
        }

      if (out_has_viewbox && (im->w <= 0 || im->w <= 0))
        {
           im->w = ceil(out_viewbox.width);
           im->h = ceil(out_viewbox.height);
           D("Choose rsvg_handle_get_intrinsic_dimensions viewbox\n");
#if !IMLIB2_DEBUG
           goto got_size;
#endif
        }
   }

#if 0
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
   {
      gdouble             dw = 0, dh = 0;

      ok = rsvg_handle_get_intrinsic_size_in_pixels(rsvg, &dw, &dh);
      D("ok=%d WxH=%.1fx%.1f\n", ok, dw, dh);
      if (ok && (im->w <= 0 || im->w <= 0))
        {
           im->w = ceil(dw);
           im->h = ceil(dh);
           D("Choose rsvg_handle_get_intrinsic_size_in_pixels width/height\n");
#if !IMLIB2_DEBUG
           goto got_size;
#endif
        }
   }
#endif
#endif

   {
      RsvgRectangle       out_ink_rect = { }, out_logical_rect = { };

      ok = rsvg_handle_get_geometry_for_element(rsvg, NULL,
                                                &out_ink_rect,
                                                &out_logical_rect, &error);
      D("ok=%d Ink: %.1f,%.1f %.1fx%.1f Log: %.1f,%.1f %.1fx%.1f\n", ok,
        out_ink_rect.x, out_ink_rect.y, out_ink_rect.width, out_ink_rect.height,
        out_logical_rect.x, out_logical_rect.y, out_logical_rect.width,
        out_logical_rect.height);
      if (ok && (im->w <= 0 || im->w <= 0))
        {
           im->w = ceil(out_ink_rect.width);
           im->h = ceil(out_ink_rect.height);
           D("Choose rsvg_handle_get_geometry_for_element ink rect width/height\n");
#if !IMLIB2_DEBUG
           goto got_size;
#endif
        }
   }

#if !IMLIB2_DEBUG
 got_size:
#endif
   if (!IMAGE_DIMENSIONS_OK(im->w, im->h))
      goto quit;

   UPDATE_FLAG(im->flags, F_HAS_ALPHA, 1);

   if (!load_data)
      QUIT_WITH_RC(LOAD_SUCCESS);

   /* Load data */

   if (!__imlib_AllocateData(im))
      QUIT_WITH_RC(LOAD_OOM);

   memset(im->data, 0, im->w * im->h * sizeof(DATA32));
   surface =
      cairo_image_surface_create_for_data((void *)im->data, CAIRO_FORMAT_ARGB32,
                                          im->w, im->h,
                                          im->w * sizeof(DATA32));;
   if (!surface)
      QUIT_WITH_RC(LOAD_OOM);

   cr = cairo_create(surface);
   if (!cr)
      QUIT_WITH_RC(LOAD_OOM);

   cvb.x = cvb.y = 0;
   cvb.width = im->w;
   cvb.height = im->h;
   rsvg_handle_render_document(rsvg, cr, &cvb, &error);

   if (im->lc)
      __imlib_LoadProgress(im, im->frame_x, im->frame_y, im->w, im->h);

   rc = LOAD_SUCCESS;

 quit:
   if (surface)
      cairo_surface_destroy(surface);
   if (cr)
      cairo_destroy(cr);
   if (rc <= 0)
      __imlib_FreeData(im);
   if (rsvg)
      g_object_unref(rsvg);
   munmap(fdata, im->fsize);

   return rc;
}

void
formats(ImlibLoader * l)
{
   static const char  *const list_formats[] = { "svg" };
   __imlib_LoaderSetFormats(l, list_formats, ARRAY_SIZE(list_formats));
}

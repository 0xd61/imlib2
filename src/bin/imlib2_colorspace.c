#include "config.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>

#include <Imlib2.h>

Display            *disp;
Window              win;

int
main(int argc, char **argv)
{
   int                 w, h, tw, th;
   Imlib_Image         im_bg = NULL;
   XEvent              ev;
   KeySym              keysym;
   Imlib_Font          font;
   Imlib_Color_Range   range;

   /**
    * First tests to determine which rendering task to perform
    */
   disp = XOpenDisplay(NULL);
   if (!disp)
     {
        fprintf(stderr, "Cannot open display\n");
        return 1;
     }

   win = XCreateSimpleWindow(disp, DefaultRootWindow(disp), 0, 0, 100, 100,
                             0, 0, 0);
   XSelectInput(disp, win,
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                PointerMotionMask | ExposureMask | KeyPressMask);

   /**
    * Start rendering
    */
   imlib_set_font_cache_size(512 * 1024);
   imlib_add_path_to_font_path(PACKAGE_DATA_DIR "/data/fonts");
   imlib_context_set_display(disp);
   imlib_context_set_visual(DefaultVisual(disp, DefaultScreen(disp)));
   imlib_context_set_colormap(DefaultColormap(disp, DefaultScreen(disp)));
   imlib_context_set_drawable(win);
   imlib_context_set_blend(0);
   imlib_context_set_color_modifier(NULL);
   imlib_context_set_blend(0);

   im_bg = imlib_create_image(600, 400);
   imlib_context_set_image(im_bg);
   w = imlib_image_get_width();
   h = imlib_image_get_height();
   imlib_context_set_color(128, 128, 255, 255);
   imlib_image_fill_rectangle(0, 0, w, h);

   XResizeWindow(disp, win, w, h);
   XMapWindow(disp, win);
   XSync(disp, False);

   while (1)
     {
        do
          {
             XNextEvent(disp, &ev);
             switch (ev.type)
               {
               case KeyPress:
                  keysym = XLookupKeysym(&ev.xkey, 0);
                  if (keysym == XK_q || keysym == XK_Escape)
                     goto quit;
                  break;
               case ButtonRelease:
                  goto quit;
               default:
                  break;

               }
          }
        while (XPending(disp));

        imlib_context_set_image(im_bg);
        imlib_context_set_color(128, 128, 255, 255);
        imlib_image_fill_rectangle(0, 0, w, h);
        imlib_context_set_color(0, 0, 0, 255);
        imlib_image_draw_rectangle(20, 20, 560, 140);
        imlib_image_draw_rectangle(20, 220, 560, 140);
        font = imlib_load_font("notepad/15");
        if (font)
          {
             char                text[4096];

             imlib_context_set_font(font);
             imlib_context_set_color(0, 0, 0, 255);
             sprintf(text, "RGBA range, 2 points, from red to magenta");
             imlib_get_text_size(text, &tw, &th);
             imlib_text_draw(300 - tw / 2, 180 - th / 2, text);
             sprintf(text, "HSVA range, 2 points, from red to magenta");
             imlib_get_text_size(text, &tw, &th);
             imlib_text_draw(300 - tw / 2, 380 - th / 2, text);
             imlib_free_font();
          }

        /* Draw rectangle w/ RGBA gradient */
        range = imlib_create_color_range();
        imlib_context_set_color_range(range);
        imlib_context_set_color(255, 0, 0, 255);
        imlib_add_color_to_color_range(0);
        imlib_context_set_color(255, 0, 255, 255);
        imlib_add_color_to_color_range(20);
        imlib_image_fill_color_range_rectangle(21, 21, 558, 138, -90.0);
        imlib_free_color_range();

        /* Draw rectangle w/ HSVA gradient */
        range = imlib_create_color_range();
        imlib_context_set_color_range(range);
        imlib_context_set_color_hsva(0, 1, 1, 255);
        imlib_add_color_to_color_range(0);
        imlib_context_set_color_hsva(300, 1, 1, 255);
        imlib_add_color_to_color_range(20);
        imlib_image_fill_hsva_color_range_rectangle(21, 221, 558, 138, -90.0);
        imlib_free_color_range();

        imlib_render_image_on_drawable(0, 0);
     }

 quit:
   return 0;
}

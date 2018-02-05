#include "common.h"
#ifdef BUILD_X11
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/Xutil.h>
#ifdef HAVE_X11_SHM_FD
#include <X11/Xlib-xcb.h>
#include <xcb/shm.h>
#include <sys/mman.h>
#endif
#include <sys/ipc.h>
#include <sys/shm.h>

#include "ximage.h"

/* global flags */
static signed char  x_does_shm = -1;

#ifdef HAVE_X11_SHM_FD
static signed char  x_does_shm_fd = 0;
#endif

/* static private variables */
static int          list_num = 0;
static XImage     **list_xim = NULL;
static XShmSegmentInfo **list_si = NULL;
static Display    **list_d = NULL;
static char        *list_used = NULL;
static int          list_mem_use = 0;
static int          list_max_mem = 1024 * 1024 * 1024;
static int          list_max_count = 0;

/* temporary X error catcher we use later */
static char         _x_err = 0;

/* the function we use for catching the error */
static int
TmpXError(Display * d, XErrorEvent * ev)
{
   _x_err = 1;
   return 0;
}

static void
ShmCheck(Display * d)
{
   const char         *s;
   int                 val;

   /* if its there set x_does_shm flag */
   if (XShmQueryExtension(d))
     {
#ifdef HAVE_X11_SHM_FD
        int                 major, minor;
        Bool                pixmaps;
#endif
        x_does_shm = 2;         /* 2: __imlib_ShmGetXImage tests first XShmAttach */
#ifdef HAVE_X11_SHM_FD
        if (XShmQueryVersion(d, &major, &minor, &pixmaps))
          {
             x_does_shm_fd = (major == 1 && minor >= 2) || major > 1;
          }
#endif
     }
   /* clear the flag - no shm at all */
   else
     {
        x_does_shm = 0;
        return;
     }

   /* Modify SHM handling if requested */
   s = getenv("IMLIB2_SHM_OPT");
   if (s)
     {
        val = atoi(s);
        if (val == 0)
           x_does_shm = x_does_shm_fd = 0;      /* Disable SHM entirely */
        else if (val == 1)
           x_does_shm_fd = 0;   /* Disable SHM-FD */

        printf("%s: x_does_shm=%d x_does_shm_fd=%d\n", __func__,
               x_does_shm, x_does_shm_fd);
     }

   /* Set ximage cache list_max_count */
   s = getenv("IMLIB2_XIMAGE_CACHE_COUNT");
   if (s)
     {
        val = atoi(s);
        if (val > 0)
           list_max_count = val;

        printf("%s: list_max_count=%d\n", __func__, list_max_count);
     }
}

XImage             *
__imlib_ShmGetXImage(Display * d, Visual * v, Drawable draw, int depth,
                     int x, int y, int w, int h, XShmSegmentInfo * si)
{
   XImage             *xim;

   if (x_does_shm < 0)
      ShmCheck(d);

   if (!x_does_shm)
      return NULL;

   /* try create an shm image */
   xim = XShmCreateImage(d, v, depth, ZPixmap, NULL, si, w, h);
   if (!xim)
      return NULL;

#ifdef HAVE_X11_SHM_FD
   if (x_does_shm_fd)
     {
        xcb_generic_error_t *error = NULL;
        xcb_shm_create_segment_cookie_t cookie;
        xcb_shm_create_segment_reply_t *reply;
        xcb_connection_t   *c = XGetXCBConnection(d);
        size_t              segment_size = xim->bytes_per_line * xim->height;

        si->shmaddr = NULL;
        si->shmseg = xcb_generate_id(c);
        si->readOnly = False;

        cookie =
           xcb_shm_create_segment(c, si->shmseg, segment_size, si->readOnly);
        reply = xcb_shm_create_segment_reply(c, cookie, &error);
        if (reply)
          {
             int                *fds;

             fds = reply->nfd == 1 ?
                xcb_shm_create_segment_reply_fds(c, reply) : NULL;
             if (fds)
               {
                  si->shmaddr = mmap(0, segment_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fds[0], 0);
                  close(fds[0]);
                  if (si->shmaddr == MAP_FAILED)
                     si->shmaddr = NULL;
               }
             if (si->shmaddr == NULL)
               {
                  xcb_shm_detach(c, si->shmseg);
               }
             free(reply);
          }
        free(error);

        if (si->shmaddr)
          {
             xim->data = si->shmaddr;
             if (draw != None)
                XShmGetImage(d, draw, xim, x, y, 0xffffffff);

             return xim;
          }
        else
          {
             x_does_shm = 0;
          }
     }
   else
#endif
     {
        /* get an shm id of this image */
        si->shmid = shmget(IPC_PRIVATE, xim->bytes_per_line * xim->height,
                           IPC_CREAT | 0666);
        /* if the get succeeds */
        if (si->shmid != -1)
          {
             /* set the params for the shm segment */
             si->readOnly = False;
             si->shmaddr = xim->data = shmat(si->shmid, 0, 0);
             /* get the shm addr for this data chunk */
             if (xim->data != (char *)-1)
               {
                  XErrorHandler       ph = NULL;

                  if (x_does_shm == 2)
                    {
                       /* setup a temporary error handler */
                       _x_err = 0;
                       XSync(d, False);
                       ph = XSetErrorHandler(TmpXError);
                    }
                  /* ask X to attach to the shared mem segment */
                  XShmAttach(d, si);
                  if (draw != None)
                     XShmGetImage(d, draw, xim, x, y, 0xffffffff);
                  if (x_does_shm == 2)
                    {
                       /* wait for X to reply and do this */
                       XSync(d, False);
                       /* reset the error handler */
                       XSetErrorHandler(ph);
                       x_does_shm = 1;
                    }

                  /* if we attached without an error we're set */
                  if (_x_err == 0)
                     return xim;

                  /* attach by X failed... must be remote client */
                  /* flag shm forever to not work - remote */
                  x_does_shm = 0;

                  /* detach */
                  shmdt(si->shmaddr);
               }

             /* get failed - out of shm id's or shm segment too big ? */
             /* remove the shm id we created */
             shmctl(si->shmid, IPC_RMID, 0);
          }
     }

   /* couldnt create SHM image ? */
   /* destroy previous image */
   XDestroyImage(xim);

   return NULL;
}

void
__imlib_ShmDestroyXImage(Display * d, XImage * xim, XShmSegmentInfo * si)
{
   XSync(d, False);
   XShmDetach(d, si);
#ifdef HAVE_X11_SHM_FD
   if (x_does_shm_fd)
     {
        munmap(si->shmaddr, xim->bytes_per_line * xim->height);
     }
   else
#endif
     {
        shmdt(si->shmaddr);
        shmctl(si->shmid, IPC_RMID, 0);
     }
   XDestroyImage(xim);
}

/* "safe" realloc allowing handling of out-of-memory situations */
static void        *
_safe_realloc(void *ptr, size_t size, int *err)
{
   void               *ptr_new;

   ptr_new = realloc(ptr, size);
   if (!ptr_new)
     {
        *err = 1;
        return ptr;
     }

   return ptr_new;
}

void
__imlib_SetMaxXImageCount(Display * d, int num)
{
   list_max_count = num;
   __imlib_FlushXImage(d);
}

int
__imlib_GetMaxXImageCount(Display * d)
{
   return list_max_count;
}

void
__imlib_SetMaxXImageTotalSize(Display * d, int num)
{
   list_max_mem = num;
   __imlib_FlushXImage(d);
}

int
__imlib_GetMaxXImageTotalSize(Display * d)
{
   return list_max_mem;
}

void
__imlib_FlushXImage(Display * d)
{
   int                 i;
   XImage             *xim;
   char                did_free = 1;

   while (((list_mem_use > list_max_mem) || (list_num > list_max_count)) &&
          (did_free))
     {
        did_free = 0;
        for (i = 0; i < list_num; i++)
          {
             if (list_used[i] == 0)
               {
                  int                 j;

                  xim = list_xim[i];
                  list_mem_use -= xim->bytes_per_line * xim->height;
                  if (list_si[i])
                    {
                       __imlib_ShmDestroyXImage(d, xim, list_si[i]);
                       free(list_si[i]);
                    }
                  else
                    {
                       XDestroyImage(xim);
                    }
                  list_num--;
                  for (j = i; j < list_num; j++)
                    {
                       list_xim[j] = list_xim[j + 1];
                       list_si[j] = list_si[j + 1];
                       list_used[j] = list_used[j + 1];
                       list_d[j] = list_d[j + 1];
                    }
                  if (list_num == 0)
                    {
                       if (list_xim)
                          free(list_xim);
                       if (list_si)
                          free(list_si);
                       if (list_used)
                          free(list_used);
                       if (list_d)
                          free(list_d);
                       list_xim = NULL;
                       list_si = NULL;
                       list_used = NULL;
                       list_d = NULL;
                    }
                  else
                    {
                       list_xim =
                          realloc(list_xim, sizeof(XImage *) * list_num);
                       list_si =
                          realloc(list_si,
                                  sizeof(XShmSegmentInfo *) * list_num);
                       list_used = realloc(list_used, sizeof(char) * list_num);
                       list_d = realloc(list_d, sizeof(Display *) * list_num);
                    }
                  did_free = 1;
               }
          }
     }
}

/* free (consume == opposite of produce) the XImage (mark as unused) */
void
__imlib_ConsumeXImage(Display * d, XImage * xim)
{
   int                 i;

   /* march through the XImage list */
   for (i = 0; i < list_num; i++)
     {
        /* find a match */
        if (list_xim[i] == xim)
          {
             /* we have a match = mark as unused */
             list_used[i] = 0;
             /* flush the XImage list to get rud of stuff we dont want */
             __imlib_FlushXImage(d);
             /* return */
             return;
          }
     }
}

/* create a new XImage or find it on our list of currently available ones so */
/* we dont need to create a new one */
XImage             *
__imlib_ProduceXImage(Display * d, Visual * v, int depth, int w, int h,
                      char *shared)
{
   XImage             *xim;
   int                 i, err;

   /* find a cached XImage (to avoid server to & fro) that is big enough */
   /* for our needs and the right depth */
   *shared = 0;
   /* go thru the current image list */
   for (i = 0; i < list_num; i++)
     {
        int                 depth_ok = 0;

        /* if the image has the same depth, width and height - recycle it */
        /* as long as its not used */
        if (list_xim[i]->depth == depth)
           depth_ok = 1;
        if (depth_ok &&
            (list_xim[i]->width >= w) && (list_xim[i]->height >= h) &&
            /*   (list_d[i] == d) && */
            (!list_used[i]))
          {
             /* mark it as used */
             list_used[i] = 1;
             /* if its shared set shared flag */
             if (list_si[i])
                *shared = 1;
             /* return it */
             return list_xim[i];
          }
     }

   /* can't find a usable XImage on the cache - create one */
   /* add the new XImage to the XImage cache */
   list_num++;
   err = 0;
   list_xim = _safe_realloc(list_xim, sizeof(XImage *) * list_num, &err);
   list_si = _safe_realloc(list_si, sizeof(XShmSegmentInfo *) * list_num, &err);
   list_used = _safe_realloc(list_used, sizeof(char) * list_num, &err);
   list_d = _safe_realloc(list_d, sizeof(Display *) * list_num, &err);
   if (err)
     {
        /* failed to allocate memory */
        list_num--;
        return NULL;
     }
   list_si[list_num - 1] = malloc(sizeof(XShmSegmentInfo));
   if (!list_si[list_num - 1])
     {
        /* failed to allocate memory */
        list_num--;
        return NULL;
     }

   /* work on making a shared image */
   xim = __imlib_ShmGetXImage(d, v, None, depth, 0, 0, w, h,
                              list_si[list_num - 1]);
   /* ok if xim == NULL it all failed - fall back to XImages */
   if (xim)
     {
        *shared = 1;
     }
   else
     {
        /* get rid of out shm info struct */
        free(list_si[list_num - 1]);
        /* flag it as NULL ot indicate a normal XImage */
        list_si[list_num - 1] = NULL;
        /* create a normal ximage */
        xim = XCreateImage(d, v, depth, ZPixmap, 0, NULL, w, h, 32, 0);
        /* allocate data for it */
        if (xim)
           xim->data = malloc(xim->bytes_per_line * xim->height);
        if (!xim || !xim->data)
          {
             /* failed to create XImage or allocate data memory */
             if (xim)
                XDestroyImage(xim);
             list_num--;
             return NULL;
          }
     }
   /* add xim to our list */
   list_xim[list_num - 1] = xim;
   /* incriment our memory count */
   list_mem_use += xim->bytes_per_line * xim->height;
   /* mark image as used */
   list_used[list_num - 1] = 1;
   /* remember what display that XImage was for */
   list_d[list_num - 1] = d;

   /* flush unused images from the image list */
   __imlib_FlushXImage(d);

   /* set the byte order of the XImage to the byte_order of the Xclient */
   /* (rather than the Xserver) */
#ifdef WORDS_BIGENDIAN
   xim->byte_order = MSBFirst;
   xim->bitmap_bit_order = MSBFirst;
#else
   xim->byte_order = LSBFirst;
   xim->bitmap_bit_order = LSBFirst;
#endif

   /* return out image */
   return xim;
}

#endif /* BUILD_X11 */

/* 
 * loader_tga.c - Loader for Truevision Targa images
 *                for Imlib2
 *
 * by Dan Maas <dmaas@dcine.com>   May 15, 2000
 *
 * based on TGA specifications available at: 
 * http://www.wotsit.org/cgi-bin/search.cgi?TGA
 *
 * header/footer structures courtesy of the GIMP Targa plugin 
 */
#include "loader_common.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "blend.h"

/* flip an inverted image - see RLE reading below */
static void         tgaflip(DATA32 * in, int w, int h);

/* TGA pixel formats */
#define TGA_TYPE_MAPPED      1
#define TGA_TYPE_COLOR       2
#define TGA_TYPE_GRAY        3
#define TGA_TYPE_MAPPED_RLE  9
#define TGA_TYPE_COLOR_RLE  10
#define TGA_TYPE_GRAY_RLE   11

/* TGA header flags */
#define TGA_DESC_ABITS      0x0f
#define TGA_DESC_HORIZONTAL 0x10
#define TGA_DESC_VERTICAL   0x20

#define TGA_SIGNATURE "TRUEVISION-XFILE"

typedef struct {
   unsigned char       idLength;
   unsigned char       colorMapType;
   unsigned char       imageType;
   unsigned char       colorMapIndexLo, colorMapIndexHi;
   unsigned char       colorMapLengthLo, colorMapLengthHi;
   unsigned char       colorMapSize;
   unsigned char       xOriginLo, xOriginHi;
   unsigned char       yOriginLo, yOriginHi;
   unsigned char       widthLo, widthHi;
   unsigned char       heightLo, heightHi;
   unsigned char       bpp;
   unsigned char       descriptor;
} tga_header;

typedef struct {
   unsigned int        extensionAreaOffset;
   unsigned int        developerDirectoryOffset;
   char                signature[16];
   char                dot;
   char                null;
} tga_footer;

/* 
 * Write an uncompressed RGBA 24- or 32-bit targa to disk
 * (If anyone wants to write a RLE saver, feel free =)
 */

char
save(ImlibImage * im, ImlibProgressFunction progress, char progress_granularity)
{
   FILE               *f;
   DATA32             *dataptr;
   unsigned char      *buf, *bufptr;
   int                 y, pl = 0;
   char                pper = 0;

   tga_header          header;

   if (!im->data)
      return 0;

   f = fopen(im->real_file, "wb");
   if (!f)
      return 0;

   /* assemble the TGA header information */

   /* most entries are zero... */
   memset(&header, 0x0, sizeof(header));

   /* uncompressed RGB Targa identifier */
   header.imageType = TGA_TYPE_COLOR;

   /* image width, low byte  */
   header.widthLo = im->w & 0xFF;
   /* image width, high byte */
   header.widthHi = im->w >> 8;

   /* image height, low byte */
   header.heightLo = im->h & 0xFF;
   /* image height, high byte */
   header.heightHi = im->h >> 8;

   /* total number of bits per pixel */
   header.bpp = (im->flags & F_HAS_ALPHA) ? 32 : 24;
   /* number of extra (alpha) bits per pixel */
   header.descriptor = (im->flags & F_HAS_ALPHA) ? 8 : 0;

   /* top-to-bottom storage */
   header.descriptor |= TGA_DESC_VERTICAL;

   /* allocate a buffer to receive the BGRA-swapped pixel values */
   buf = malloc(im->w * im->h * ((im->flags & F_HAS_ALPHA) ? 4 : 3));
   if (!buf)
     {
        fclose(f);
        return 0;
     }

   /* now we have to read from im->data into buf, swapping RGBA to BGRA */
   dataptr = im->data;
   bufptr = buf;

   /* for each row */
   for (y = 0; y < im->h; y++)
     {
        int                 x;

        /* for each pixel in the row */
        for (x = 0; x < im->w; x++)
          {
             DATA32              pixel = *dataptr++;

             *bufptr++ = PIXEL_B(pixel);
             *bufptr++ = PIXEL_G(pixel);
             *bufptr++ = PIXEL_R(pixel);
             if (im->flags & F_HAS_ALPHA)
                *bufptr++ = PIXEL_A(pixel);
          }                     /* end for (each pixel in row) */

        /* report progress every row */
        if (progress)
          {
             char                per;
             int                 l;

             per = (char)((100 * y) / im->h);
             if (((per - pper) >= progress_granularity) || (y == (im->h - 1)))
               {
                  l = y - pl;
                  if (!progress(im, per, 0, (y - l), im->w, l))
                    {
                       free(buf);
                       fclose(f);
                       return 2;
                    }
                  pper = per;
                  pl = y;
               }
          }
     }

   /* write the header */
   fwrite(&header, sizeof(header), 1, f);

   /* write the image data */
   fwrite(buf, 1, im->w * im->h * ((im->flags & F_HAS_ALPHA) ? 4 : 3), f);

   free(buf);
   fclose(f);
   return 1;
}

/* Load up a TGA file 
 * 
 * As written this function only recognizes the following types of Targas: 
 *		Type 02 - Uncompressed RGB, 24 or 32 bits 
 *		Type 03 - Uncompressed grayscale, 8 bits
 *		Type 10 - RLE-compressed RGB, 24 or 32 bits
 *		Type 11 - RLE-compressed grayscale, 8 bits  
 * There are several other (uncommon) Targa formats which this function can't currently handle
 */

char
load(ImlibImage * im, ImlibProgressFunction progress,
     char progress_granularity, char immediate_load)
{
   int                 fd, rc;
   void               *seg, *filedata;
   struct stat         ss;
   int                 bpp, vinverted = 0;
   int                 rle = 0, footer_present = 0;

   tga_header         *header;
   tga_footer         *footer;

   unsigned long       datasize;
   unsigned char      *bufptr, *bufend;
   DATA32             *dataptr;

   int                 y;

   fd = open(im->real_file, O_RDONLY);
   if (fd < 0)
      return 0;

   rc = 0;                      /* Error */
   seg = MAP_FAILED;

   if (fstat(fd, &ss) < 0)
      goto quit;

   if (ss.st_size < (long)(sizeof(tga_header) + sizeof(tga_footer)) ||
       (uintmax_t) ss.st_size > SIZE_MAX)
      goto quit;

   seg = mmap(0, ss.st_size, PROT_READ, MAP_SHARED, fd, 0);
   if (seg == MAP_FAILED)
      goto quit;

   filedata = seg;
   header = (tga_header *) filedata;
   footer = (tga_footer *) ((char *)filedata + ss.st_size - sizeof(tga_footer));

   /* check the footer to see if we have a v2.0 TGA file */
   if (memcmp(footer->signature, TGA_SIGNATURE, sizeof(footer->signature)) == 0)
      footer_present = 1;

   if (!footer_present)
     {
     }

   if ((size_t)ss.st_size < sizeof(tga_header) + header->idLength +
       (footer_present ? sizeof(tga_footer) : 0))
      goto quit;

   /* skip over header */
   filedata = (char *)filedata + sizeof(tga_header);

   /* skip over alphanumeric ID field */
   if (header->idLength)
      filedata = (char *)filedata + header->idLength;

   /* now parse the header */

   /* this flag indicated bottom-up pixel storage */
   vinverted = !(header->descriptor & TGA_DESC_VERTICAL);

   switch (header->imageType)
     {
     case TGA_TYPE_COLOR_RLE:
     case TGA_TYPE_GRAY_RLE:
        rle = 1;
        break;

     case TGA_TYPE_COLOR:
     case TGA_TYPE_GRAY:
        rle = 0;
        break;

     default:
        goto quit;
     }

   /* bits per pixel */
   bpp = header->bpp;

   if (!((bpp == 32) || (bpp == 24) || (bpp == 8)))
      goto quit;

   /* endian-safe loading of 16-bit sizes */
   im->w = (header->widthHi << 8) | header->widthLo;
   im->h = (header->heightHi << 8) | header->heightLo;

   if (!IMAGE_DIMENSIONS_OK(im->w, im->h))
      goto quit;

   if (bpp == 32)
      SET_FLAG(im->flags, F_HAS_ALPHA);
   else
      UNSET_FLAG(im->flags, F_HAS_ALPHA);

   if (!(im->loader || immediate_load || progress))
     {
        rc = 1;
        goto quit;
     }

   /* Load data */

   /* allocate the destination buffer */
   if (!__imlib_AllocateData(im, im->w, im->h))
      goto quit;

   /* first we read the file data into a buffer for parsing */
   /* then we decode from RAM */

   /* find out how much data must be read from the file */
   /* (this is NOT simply width*height*4, due to compression) */

   datasize = ss.st_size - sizeof(tga_header) - header->idLength -
      (footer_present ? sizeof(tga_footer) : 0);

   /* buffer is ready for parsing */

   /* bufptr is the next byte to be read from the buffer */
   bufptr = filedata;
   bufend = bufptr + datasize;

   /* dataptr is the next 32-bit pixel to be filled in */
   dataptr = im->data;

   if (!rle)
     {
        /* decode uncompressed BGRA data */
        for (y = 0; y < im->h; y++)     /* for each row */
          {
             int                 x;

             /* point dataptr at the beginning of the row */
             if (vinverted)
                /* some TGA's are stored upside-down! */
                dataptr = im->data + ((im->h - y - 1) * im->w);
             else
                dataptr = im->data + (y * im->w);

             for (x = 0; (x < im->w); x++)      /* for each pixel in the row */
               {
                  if (bufptr + bpp / 8 > bufend)
                     goto quit;

                  switch (bpp)
                    {
                    case 32:   /* 32-bit BGRA pixels */
                       *dataptr++ =
                          PIXEL_ARGB(bufptr[3], bufptr[2], bufptr[1],
                                     bufptr[0]);
                       bufptr += 4;
                       break;

                    case 24:   /* 24-bit BGR pixels */
                       *dataptr++ =
                          PIXEL_ARGB(0xff, bufptr[2], bufptr[1], bufptr[0]);
                       bufptr += 3;
                       break;

                    case 8:    /* 8-bit grayscale */
                       *dataptr++ =
                          PIXEL_ARGB(0xff, bufptr[0], bufptr[0], bufptr[0]);
                       bufptr += 1;
                       break;
                    }

               }                /* end for (each pixel) */
          }
     }
   else
     {
        /* decode RLE compressed data */
        unsigned char       curbyte, red, green, blue, alpha;
        DATA32             *final_pixel = dataptr + im->w * im->h;

        /* loop until we've got all the pixels or run out of input */
        while ((dataptr < final_pixel))
          {
             int                 i, count;

             if ((bufptr + 1 + (bpp / 8)) > bufend)
                goto quit;

             curbyte = *bufptr++;
             count = (curbyte & 0x7F) + 1;

             if (curbyte & 0x80)        /* RLE packet */
               {
                  switch (bpp)
                    {
                    case 32:
                       blue = *bufptr++;
                       green = *bufptr++;
                       red = *bufptr++;
                       alpha = *bufptr++;
                       for (i = 0; (i < count) && (dataptr < final_pixel); i++)
                          *dataptr++ = PIXEL_ARGB(alpha, red, green, blue);
                       break;

                    case 24:
                       blue = *bufptr++;
                       green = *bufptr++;
                       red = *bufptr++;
                       alpha = 0xff;
                       for (i = 0; (i < count) && (dataptr < final_pixel); i++)
                          *dataptr++ = PIXEL_ARGB(alpha, red, green, blue);
                       break;

                    case 8:
                       red = *bufptr++;
                       alpha = 0xff;
                       for (i = 0; (i < count) && (dataptr < final_pixel); i++)
                          *dataptr++ = PIXEL_ARGB(alpha, red, red, red);
                       break;
                    }
               }                /* end if (RLE packet) */
             else               /* raw packet */
               {
                  for (i = 0; (i < count) && (dataptr < final_pixel); i++)
                    {
                       if ((bufptr + bpp / 8) > bufend)
                          goto quit;

                       switch (bpp)
                         {
                         case 32:      /* 32-bit BGRA pixels */
                            *dataptr++ =
                               PIXEL_ARGB(bufptr[3], bufptr[2], bufptr[1],
                                          bufptr[0]);
                            bufptr += 4;
                            break;

                         case 24:      /* 24-bit BGR pixels */
                            *dataptr++ =
                               PIXEL_ARGB(0xff, bufptr[2], bufptr[1],
                                          bufptr[0]);
                            bufptr += 3;
                            break;

                         case 8:       /* 8-bit grayscale */
                            *dataptr++ =
                               PIXEL_ARGB(0xff, bufptr[0], bufptr[0],
                                          bufptr[0]);
                            bufptr += 1;
                            break;
                         }
                    }
               }                /* end if (raw packet) */
          }                     /* end for (each packet) */

        /* must now flip a bottom-up image */
        if (vinverted)
           tgaflip(im->data, im->w, im->h);
     }

   if (progress)
     {
        progress(im, 100, 0, 0, im->w, im->h);
     }

   rc = 1;                      /* Success */

 quit:
   if (rc == 0)
      __imlib_FreeData(im);
   if (seg != MAP_FAILED)
      munmap(seg, ss.st_size);
   close(fd);
   return rc;
}

void
formats(ImlibLoader * l)
{
   static const char  *const list_formats[] = { "tga" };
   int                 i;

   l->num_formats = sizeof(list_formats) / sizeof(char *);
   l->formats = malloc(sizeof(char *) * l->num_formats);

   for (i = 0; i < l->num_formats; i++)
      l->formats[i] = strdup(list_formats[i]);
}

/**********************/

/* flip a DATA32 image block vertically in place */

static void
tgaflip(DATA32 * in, int w, int h)
{
   DATA32             *adv, *adv2;
   int                 x, y;

   adv = in;
   adv2 = in + (w * (h - 1));

   for (y = 0; y < (h / 2); y++)
     {
        DATA32              tmp;

        for (x = 0; x < w; x++)
          {
             tmp = adv[x];
             adv[x] = adv2[x];
             adv2[x] = tmp;
          }
        adv2 -= w;
        adv += w;
     }
}

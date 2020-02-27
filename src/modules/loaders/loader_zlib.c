#include "loader_common.h"
#include <zlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define OUTBUF_SIZE 16484

static int
uncompress_file(FILE * fp, int dest)
{
   gzFile              gf;
   DATA8               outbuf[OUTBUF_SIZE];
   int                 ret = 1, bytes;

   gf = gzdopen(dup(fileno(fp)), "rb");
   if (!gf)
      return 0;

   while (1)
     {
        bytes = gzread(gf, outbuf, OUTBUF_SIZE);

        if (!bytes)
           break;
        else if (bytes == -1)
          {
             ret = 0;
             break;
          }
        else if (write(dest, outbuf, bytes) != bytes)
           break;
     }

   gzclose(gf);

   return ret;
}

char
load(ImlibImage * im, ImlibProgressFunction progress,
     char progress_granularity, char load_data)
{
   ImlibLoader        *loader;
   FILE               *fp;
   int                 dest, res;
   const char         *s, *p, *q;
   char                tmp[] = "/tmp/imlib2_loader_zlib-XXXXXX";
   char               *real_ext;

   /* make sure this file ends in ".gz" and that there's another ext
    * (e.g. "foo.png.gz") */
   for (p = s = im->real_file, q = NULL; *s; s++)
     {
        if (*s != '.' && *s != '/')
           continue;
        q = p;
        p = s + 1;
     }
   if (!q || strcasecmp(p, "gz"))
      return 0;

   if (!(real_ext = strndup(q, p - q - 1)))
      return 0;

   loader = __imlib_FindBestLoaderForFormat(real_ext, 0);
   free(real_ext);
   if (!loader)
      return 0;

   if (!(fp = fopen(im->real_file, "rb")))
      return 0;

   if ((dest = mkstemp(tmp)) < 0)
     {
        fclose(fp);
        return 0;
     }

   res = uncompress_file(fp, dest);
   fclose(fp);
   close(dest);

   if (!res)
      goto quit;

   res = __imlib_LoadEmbedded(loader, im, tmp, load_data);

 quit:
   unlink(tmp);

   return res;
}

void
formats(ImlibLoader * l)
{
   static const char  *const list_formats[] = { "gz" };
   __imlib_LoaderSetFormats(l, list_formats,
                            sizeof(list_formats) / sizeof(char *));
}

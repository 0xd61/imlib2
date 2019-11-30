#include "loader_common.h"
#include <bzlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define OUTBUF_SIZE 16384
#define INBUF_SIZE 1024

static int
uncompress_file(FILE * fp, int dest)
{
   BZFILE             *bf;
   DATA8               outbuf[OUTBUF_SIZE];
   int                 bytes, error, ret = 1;

   bf = BZ2_bzReadOpen(&error, fp, 0, 0, NULL, 0);

   if (error != BZ_OK)
     {
        BZ2_bzReadClose(NULL, bf);
        return 0;
     }

   while (1)
     {
        bytes = BZ2_bzRead(&error, bf, &outbuf, OUTBUF_SIZE);

        if (error == BZ_OK || error == BZ_STREAM_END)
           if (write(dest, outbuf, bytes) != bytes)
              break;

        if (error == BZ_STREAM_END)
           break;
        else if (error != BZ_OK)
          {
             ret = 0;
             break;
          }
     }

   BZ2_bzReadClose(&error, bf);

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
   char                tmp[] = "/tmp/imlib2_loader_bz2-XXXXXX";
   char               *file, *real_ext;

   /* make sure this file ends in ".bz2" and that there's another ext
    * (e.g. "foo.png.bz2") */
   for (s = im->real_file, p = q = NULL; *s; s++)
     {
        if (*s != '.')
           continue;
        q = p;
        p = s;
     }
   if (!q || q == im->real_file || strcasecmp(p + 1, "bz2"))
      return 0;

   if (!(real_ext = strndup(q + 1, p - q - 1)))
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
     {
        unlink(tmp);
        return 0;
     }

   /* remember the original filename */
   file = im->real_file;
   im->real_file = strdup(tmp);

   loader->load(im, progress, progress_granularity, load_data);

   free(im->real_file);
   im->real_file = file;

   unlink(tmp);

   return 1;
}

void
formats(ImlibLoader * l)
{
   static const char  *const list_formats[] = { "bz2" };
   int                 i;

   l->num_formats = sizeof(list_formats) / sizeof(char *);
   l->formats = malloc(sizeof(char *) * l->num_formats);

   for (i = 0; i < l->num_formats; i++)
      l->formats[i] = strdup(list_formats[i]);
}

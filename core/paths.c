#include <string.h>
#include "paths.h"

#ifdef _WIN32
#define PATH_SLASH "\\"
#else
#define PATH_SLASH "/"
#endif

/* @s after the @*len bytes of the path so far, as much of it as @size has
 * room for with the zero at the end; @*len goes on counting the rest */
static void path_cat(char *out, size_t size, size_t *len, const char *s)
{
   size_t n = strlen(s);
   if (*len + 1 < size)
   {
      size_t room = size - 1 - *len;
      size_t c    = n < room ? n : room;
      memcpy(out + *len, s, c);
      out[*len + c] = '\0';
   }
   *len += n;
}

size_t writable_data_path(char *out, size_t size, const char *filename)
{
   size_t len = 0;
   if (size)
      out[0] = '\0';
   path_cat(out, size, &len, game_dir_no_slash);
   path_cat(out, size, &len, PATH_SLASH);
   path_cat(out, size, &len, filename);
   return len;
}

size_t writable_vmu_path(char *out, size_t size, const char *logical_port)
{
   size_t len = 0;
   if (size)
      out[0] = '\0';
   /* (each piece where it goes: a content's name is as long as it is) */
   if ((per_content_vmus == 1 && !strcmp("A1", logical_port)) ||
       (per_content_vmus == 2))
   {
      path_cat(out, size, &len, vmu_dir_no_slash);
      path_cat(out, size, &len, PATH_SLASH);
      path_cat(out, size, &len, content_name);
      path_cat(out, size, &len, ".");
   }
   else
   {
      path_cat(out, size, &len, game_dir_no_slash);
      path_cat(out, size, &len, PATH_SLASH);
      path_cat(out, size, &len, "vmu_save_");
   }
   path_cat(out, size, &len, logical_port);
   path_cat(out, size, &len, ".bin");
   return len;
}

/* The content's discs: see lr_disk.h */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <compat/strl.h>
#include <retro_miscellaneous.h>
#include "lr_disk.h"
#include "emu_baton.h"
#include "types.h"
#include "imgread/common.h"
#include "log/Log.h"

#ifndef PATH_MAX
#define PATH_MAX PATH_MAX_LENGTH
#endif

/* A disc of the list: its path and its label, NULL for none (an index
 * the frontend added and has not filled) */
typedef struct lr_disk
{
   char *path;
   char *label;
} lr_disk;

unsigned disk_index = 0;
bool disc_tray_open = false;

/* The list: made as it is read or the frontend adds to it, kept until
 * the content goes */
static lr_disk *disks;
static unsigned disks_count, disks_room;
/* The disc the frontend says it last had in */
static unsigned disk_initial_index = 0;
static char *disk_initial_path;

static struct retro_disk_control_callback retro_disk_control_cb;
static struct retro_disk_control_ext_callback retro_disk_control_ext_cb;

void short_pathname_representation(char *out, const char *path, size_t size)
{
   fill_pathname_base(out, path, size);
   path_remove_extension(out);
}

static char *dup_or_null(const char *s)
{
   size_t n;
   char *d;
   if (s == NULL || s[0] == '\0')
      return NULL;
   n = strlen(s) + 1;
   d = (char *)malloc(n);
   if (d != NULL)
      memcpy(d, s, n);
   return d;
}

static void disk_free(lr_disk *d)
{
   free(d->path);
   free(d->label);
   d->path = NULL;
   d->label = NULL;
}

void lr_disk_clear(void)
{
   unsigned i;
   for (i = 0; i < disks_count; i++)
      disk_free(&disks[i]);
   disks_count = 0;
   disk_index  = 0;
}

unsigned lr_disk_count(void)
{
   return disks_count;
}

/* A disc put at the end of the list; false for no memory */
static bool disk_push(const char *path, const char *label)
{
   lr_disk *d;
   if (disks_count == disks_room)
   {
      unsigned room = disks_room != 0 ? disks_room * 2 : 8;
      lr_disk *more = (lr_disk *)realloc(disks, room * sizeof(*disks));
      if (more == NULL)
         return false;
      disks      = more;
      disks_room = room;
   }
   d        = &disks[disks_count];
   d->path  = dup_or_null(path);
   d->label = dup_or_null(label);
   if ((path != NULL && path[0] != '\0' && d->path == NULL)
         || (label != NULL && label[0] != '\0' && d->label == NULL))
   {
      disk_free(d);
      return false;
   }
   disks_count++;
   return true;
}

/* The disc the drive is to read when its lid shuts: the one at disk_index,
 * or none past the end of the list (or at an index added and not filled). */
static bool disk_select_image(void)
{
   settings.imgread.LoadDefaultImage = true;
   if (disk_index >= disks_count || disks[disk_index].path == NULL)
   {
      settings.imgread.DefaultImage[0] = '\0';
      return false;
   }
   strncpy(settings.imgread.DefaultImage, disks[disk_index].path, sizeof(settings.imgread.DefaultImage));
   settings.imgread.DefaultImage[sizeof(settings.imgread.DefaultImage) - 1] = '\0';
   return true;
}

/* The lid shut on the disc at disk_index. Only a Dreamcast has a drive. */
static bool disk_close_lid(void)
{
   bool ok = true;

#if !defined(TARGET_NO_THREADS)
   /* (a state may be being saved on the frontend's other thread) */
   emu_baton_hold(&emu_baton);
#endif
   if (disk_select_image())
      ok = DiscSwap(0);
   else
      DiscCloseEmpty();
#if !defined(TARGET_NO_THREADS)
   emu_baton_release(&emu_baton);
#endif
   return ok;
}

static bool retro_set_eject_state(bool ejected)
{
   if (!game_loaded || settings.System != DC_PLATFORM_DREAMCAST)
      return false;
   if (ejected == disc_tray_open)
      return true;
   disc_tray_open = ejected;
   if (ejected)
   {
#if !defined(TARGET_NO_THREADS)
      emu_baton_hold(&emu_baton);
#endif
      DiscOpenLid();
#if !defined(TARGET_NO_THREADS)
      emu_baton_release(&emu_baton);
#endif
      return true;
   }
   return disk_close_lid();
}

static bool retro_get_eject_state(void)
{
   return disc_tray_open;
}

static unsigned retro_get_image_index(void)
{
   return disk_index;
}

static bool retro_set_image_index(unsigned index)
{
   if (index > disks_count)
      return false;
   if (game_loaded && settings.System != DC_PLATFORM_DREAMCAST)
      return false;
   disk_index = index;
   if (!game_loaded)
      return true;
   if (disc_tray_open)
   {
      disk_select_image();
      return true;
   }
   return disk_close_lid();
}

static unsigned retro_get_num_images(void)
{
   return disks_count;
}

static bool retro_add_image_index(void)
{
   return disk_push(NULL, NULL);
}

static bool retro_replace_image_index(unsigned index, const struct retro_game_info *info)
{
   if (index >= disks_count)
      return false;

   if (info == NULL)
   {
      disk_free(&disks[index]);
      memmove(&disks[index], &disks[index + 1], (disks_count - index - 1) * sizeof(*disks));
      disks_count--;

      if ((disk_index >= index) && (disk_index > 0))
         disk_index--;
   }
   else
   {
      static char disk_label[PATH_MAX];
      char *path, *label;

      disk_label[0] = '\0';
      short_pathname_representation(disk_label, info->path, sizeof(disk_label));
      path  = dup_or_null(info->path);
      label = dup_or_null(disk_label);
      if ((info->path[0] != '\0' && path == NULL) || (disk_label[0] != '\0' && label == NULL))
      {
         free(path);
         free(label);
         return false;
      }
      disk_free(&disks[index]);
      disks[index].path  = path;
      disks[index].label = label;
   }

   return true;
}

static bool retro_set_initial_image(unsigned index, const char *path)
{
   if (!path || (*path == '\0'))
      return false;

   disk_initial_index = index;
   free(disk_initial_path);
   disk_initial_path = dup_or_null(path);
   if (disk_initial_path == NULL)
      return false;

   return true;
}

static bool retro_get_image_path(unsigned index, char *path, size_t len)
{
   if (len < 1)
      return false;

   if (index >= disks_count)
      return false;

   if (disks[index].path == NULL)
      return false;

   strncpy(path, disks[index].path, len - 1);
   path[len - 1] = '\0';

   return true;
}

static bool retro_get_image_label(unsigned index, char *label, size_t len)
{
   if (len < 1)
      return false;

   if (index >= disks_count)
      return false;

   if (disks[index].label == NULL)
      return false;

   strncpy(label, disks[index].label, len - 1);
   label[len - 1] = '\0';

   return true;
}

void lr_disk_init_interface(retro_environment_t env)
{
   unsigned dci_version = 0;

   retro_disk_control_cb.set_eject_state     = retro_set_eject_state;
   retro_disk_control_cb.get_eject_state     = retro_get_eject_state;
   retro_disk_control_cb.set_image_index     = retro_set_image_index;
   retro_disk_control_cb.get_image_index     = retro_get_image_index;
   retro_disk_control_cb.get_num_images      = retro_get_num_images;
   retro_disk_control_cb.add_image_index     = retro_add_image_index;
   retro_disk_control_cb.replace_image_index = retro_replace_image_index;

   retro_disk_control_ext_cb.set_eject_state     = retro_set_eject_state;
   retro_disk_control_ext_cb.get_eject_state     = retro_get_eject_state;
   retro_disk_control_ext_cb.set_image_index     = retro_set_image_index;
   retro_disk_control_ext_cb.get_image_index     = retro_get_image_index;
   retro_disk_control_ext_cb.get_num_images      = retro_get_num_images;
   retro_disk_control_ext_cb.add_image_index     = retro_add_image_index;
   retro_disk_control_ext_cb.replace_image_index = retro_replace_image_index;
   retro_disk_control_ext_cb.set_initial_image   = retro_set_initial_image;
   retro_disk_control_ext_cb.get_image_path      = retro_get_image_path;
   retro_disk_control_ext_cb.get_image_label     = retro_get_image_label;

   disk_initial_index = 0;
   free(disk_initial_path);
   disk_initial_path  = NULL;
   if (env(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dci_version) && (dci_version >= 1))
      env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &retro_disk_control_ext_cb);
   else
      env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &retro_disk_control_cb);
}

char *lr_disk_start(const char *content)
{
   static char disk_label[PATH_MAX];
   const char *path;

   /* A playlist read: the game starts from its discs */
   if (disks_count > 0)
   {
      disk_index = 0;

      /* Attempt to set initial disk index */
      if ((disks_count > 1) &&
          (disk_initial_index > 0) &&
          (disk_initial_index < disks_count))
         if (strcmp(disks[disk_initial_index].path != NULL ? disks[disk_initial_index].path : "",
                  disk_initial_path != NULL ? disk_initial_path : "") == 0)
            disk_index = disk_initial_index;

      path = disks[disk_index].path != NULL ? disks[disk_index].path : "";
   }
   else
   {
      disk_label[0] = '\0';
      short_pathname_representation(disk_label, content, sizeof(disk_label));
      if (!disk_push(content, disk_label))
         return NULL;
      path = content;
   }
   {
      size_t n = strlen(path) + 1;
      char *copy = (char *)malloc(n);
      if (copy != NULL)
         memcpy(copy, path, n);
      return copy;
   }
}

/* (a line's leading and trailing blanks, and its end of line, taken off) */
static char *m3u_trim(char *s)
{
   size_t len;

   while (*s == ' ' || *s == '\t')
      s++;
   len = strlen(s);
   while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'
            || s[len - 1] == ' ' || s[len - 1] == '\t'))
      s[--len] = '\0';
   return s;
}

/* An m3u as RetroArch writes and reads one: a disc's path on each line,
 * relative to the playlist or absolute, quoted or not, with its label
 * after a '|' or on an "#EXTINF:<runtime>,<label>" or "#LABEL:<label>"
 * line before it; other lines starting with '#' and blank lines are
 * skipped, and so is a UTF-8 byte order mark. (Its lines read into room
 * of its own: a load is on one thread.) */
bool lr_disk_read_m3u(const char *file, const char *dir)
{
   static char line[PATH_MAX];
   static char name[PATH_MAX];
   static char next_label[PATH_MAX];
   static char disk_label[PATH_MAX];
   RFILE *f = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   /* The discs read from this file: the disc in the drive (disk_index) is
    * chosen once they are all read. */
   unsigned count = 0;
   bool first = true;

   if (!f)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Could not read file\n");
      return false;
   }

   next_label[0] = '\0';
   while (filestream_gets(f, line, sizeof(line)))
   {
      char *entry = line;
      char *bar;
      size_t len;

      if (first && (u8)entry[0] == 0xEF && (u8)entry[1] == 0xBB && (u8)entry[2] == 0xBF)
         entry += 3;
      first = false;
      entry = m3u_trim(entry);

      if (entry[0] == '#')
      {
         const char *label = NULL;

         if (!strncmp(entry, "#EXTINF:", 8))
         {
            label = strchr(entry + 8, ',');
            if (label)
               label++;
         }
         else if (!strncmp(entry, "#LABEL:", 7))
            label = entry + 7;
         if (label)
            strlcpy(next_label, m3u_trim((char *)label), sizeof(next_label));
         continue;
      }

      bar = strchr(entry, '|');
      if (bar)
      {
         *bar = '\0';
         strlcpy(next_label, m3u_trim(bar + 1), sizeof(next_label));
         entry = m3u_trim(entry);
      }

      /* Remove any beginning and ending quotes as these can cause issues when feeding the paths into command line later */
      len = strlen(entry);
      if (len >= 2 && entry[0] == '"' && entry[len - 1] == '"')
      {
         entry[len - 1] = '\0';
         entry = m3u_trim(entry + 1);
      }

      if (entry[0] != '\0')
      {
         if (path_is_absolute(entry))
            strlcpy(name, entry, sizeof(name));
         else
         {
            strlcpy(name, dir, sizeof(name));
            strlcat(name, entry, sizeof(name));
         }

         if (next_label[0] != '\0')
            strlcpy(disk_label, next_label, sizeof(disk_label));
         else
            short_pathname_representation(disk_label, name, sizeof(disk_label));
         if (!disk_push(name, disk_label))
            break;
         next_label[0] = '\0';

         count++;
      }
   }

   filestream_close(f);
   return (count != 0);
}

/* Which of the content's discs was in the drive, and whether its lid was
 * open, follows the machine's state: without it a state made with the
 * second disc in was loaded with the first in the drive and read from it.
 * Words of 0 (an older state) leave the drive as it is. */
#define DISC_TRAILER_MAGIC 0x43534944   /* "DISC" */
#define DISC_TRAILER_OPEN  1
#define DISC_TRAILER_IN    2

void lr_disk_trailer_write(u8 *at)
{
   u32 words[4] = { 0, 0, 0, 0 };

   if (settings.System == DC_PLATFORM_DREAMCAST && disks_count != 0)
   {
      words[0] = DISC_TRAILER_MAGIC;
      words[1] = 1;
      words[2] = disk_index;
      words[3] = (disc_tray_open ? DISC_TRAILER_OPEN : 0)
         | (libGDR_DiscPresent() ? DISC_TRAILER_IN : 0);
   }
   memcpy(at, words, sizeof(words));
}

void lr_disk_trailer_read(const u8 *at)
{
   u32 words[4];
   bool in, open;

   memcpy(words, at, sizeof(words));
   if (words[0] != DISC_TRAILER_MAGIC || words[1] != 1
         || settings.System != DC_PLATFORM_DREAMCAST)
      return;
   open = (words[3] & DISC_TRAILER_OPEN) != 0;
   in   = (words[3] & DISC_TRAILER_IN) != 0;
   if (words[2] > disks_count || (in && (words[2] == disks_count
               || disks[words[2]].path == NULL)))
      return;
   /* Nothing to do when the same disc is in, which with run-ahead and
    * rewind is every load but those across a disc change. */
   if (in == libGDR_DiscPresent() && words[2] == disk_index && open == disc_tray_open)
      return;
   disk_index     = words[2];
   disc_tray_open = open;
   if (in)
      in = disk_select_image() && DiscPutBack(settings.imgread.DefaultImage);
   else
   {
      disk_select_image();
      TermDrive();
   }
   if (!in && (words[3] & DISC_TRAILER_IN))
      WARN_LOG(GDROM, "The state's disc \"%s\" could not be opened\n", settings.imgread.DefaultImage);
}

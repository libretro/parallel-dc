/* The content's discs (core/libretro/lr_disk.c), driven at random:
 * playlists of every shape read (labels, quotes, byte order marks, blank
 * and comment lines, absolute and relative paths), content without one,
 * the frontend's disc control asked everything in any order (discs added,
 * filled, taken out, an initial disc, the lid opened and shut, before and
 * after a game is loaded, on every machine), and the disc's words in a
 * state written and read back changed. Every call out of it, and what it
 * answers, goes into a hash printed each round. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libretro.h>
#include <streams/file_stream.h>
#include "types.h"
#include "libretro/lr_disk.h"
#include "libretro/emu_baton.h"
#include "imgread/common.h"

settings_t settings;
emu_baton_t emu_baton;
retro_log_printf_t log_cb;
bool game_loaded;

static u32 hash = 2166136261u;
static void mix(u32 v)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      hash ^= v & 0xff;
      hash *= 16777619u;
      v >>= 8;
   }
}
static void mixs(const char *s)
{
   if (s == NULL)
   {
      mix(0xdead);
      return;
   }
   while (*s)
      mix((u8)*s++);
   mix(0x100);
}

static u32 rng = 7;
static u32 rnd(void)
{
   rng ^= rng << 13;
   rng ^= rng >> 17;
   rng ^= rng << 5;
   return rng;
}

static int held;
void emu_baton_hold(emu_baton_t *b)
{
   (void)b;
   mix(0x6100);
   held++;
}
void emu_baton_release(emu_baton_t *b)
{
   (void)b;
   mix(0x6101);
   held--;
}

static bool present;
static bool swap_fails;
bool DiscSwap(u32 fileflags)
{
   mix(0x6200 | fileflags);
   mix(held);
   mixs(settings.imgread.DefaultImage);
   present = !swap_fails;
   return !swap_fails;
}
void DiscOpenLid(void)
{
   mix(0x6201);
   mix(held);
   present = false;
}
void DiscCloseEmpty(void)
{
   mix(0x6202);
   mix(held);
   present = false;
}
bool DiscPutBack(const char *fn)
{
   mix(0x6203);
   mixs(fn);
   present = !swap_fails;
   return !swap_fails;
}
void TermDrive(void)
{
   mix(0x6204);
   present = false;
}
bool libGDR_DiscPresent(void)
{
   mix(0x6205);
   return present;
}

/* The files: one playlist made in memory, read a line at a time as
 * filestream_gets() does */
static char m3u[4096];
static size_t m3u_len, m3u_pos;
static bool m3u_missing;
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
   mixs(path);
   mix(mode);
   mix(hints);
   if (m3u_missing)
      return NULL;
   m3u_pos = 0;
   return (RFILE *)m3u;
}
char *filestream_gets(RFILE *stream, char *s, size_t len)
{
   size_t n = 0;
   (void)stream;
   if (m3u_pos >= m3u_len || len == 0)
      return NULL;
   while (n + 1 < len && m3u_pos < m3u_len)
   {
      char c = m3u[m3u_pos++];
      s[n++] = c;
      if (c == '\n')
         break;
   }
   s[n] = '\0';
   return s;
}
int filestream_close(RFILE *stream)
{
   (void)stream;
   mix(0x6300);
   return 0;
}

static void log_fake(enum retro_log_level level, const char *fmt, ...)
{
   mix(0x6400 | level);
   mixs(fmt);
}

/* The frontend: it is given the disc control, the newer one or the older */
static struct retro_disk_control_ext_callback dc;
static bool has_ext;
static bool env_fake(unsigned cmd, void *data)
{
   mix(0x6500 | cmd);
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
      *(unsigned *)data = has_ext ? 1 : 0;
      return true;
   case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
      memcpy(&dc, data, sizeof(dc));
      return true;
   case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
      memset(&dc, 0, sizeof(dc));
      memcpy(&dc, data, sizeof(struct retro_disk_control_callback));
      return true;
   }
   return false;
}

static const char *const pieces[] = {
   "disc1.gdi", "Disc 2.chd", "/abs/path/disc3.cdi", "sub/dir/four.gdi", "../up/five.chd",
   "\"quoted name.gdi\"", "  spaced.chd  ", "\" both \"", "x", "noext", "a.b.c.d",
   "\"", "\t tabbed.gdi\t", "\xEF\xBB\xBF" "bom.gdi"
};
static const char *const labels[] = { "First", "  Second  ", "", "Third|odd", "Lbl" };

static void make_m3u(void)
{
   int lines = (int)(rnd() % 9);
   int i;
   m3u_len = 0;
   if (rnd() % 4 == 0)
   {
      memcpy(m3u, "\xEF\xBB\xBF", 3);
      m3u_len = 3;
   }
   for (i = 0; i < lines; i++)
   {
      char buf[256];
      const char *p = pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))];
      const char *l = labels[rnd() % (sizeof(labels) / sizeof(labels[0]))];
      const char *eol = rnd() % 3 == 0 ? "\r\n" : "\n";
      switch (rnd() % 8)
      {
      case 0: sprintf(buf, "#EXTINF:%u,%s%s", (unsigned)(rnd() % 100), l, eol); break;
      case 1: sprintf(buf, "#LABEL:%s%s", l, eol); break;
      case 2: sprintf(buf, "%s|%s%s", p, l, eol); break;
      case 3: sprintf(buf, "# a comment%s", eol); break;
      case 4: sprintf(buf, "   %s", eol); break;
      case 5: sprintf(buf, "#EXTINF:no comma%s", eol); break;
      default: sprintf(buf, "%s%s", p, eol); break;
      }
      if (rnd() % 10 == 0)
         buf[strlen(buf) - 1] = '\0';   /* (the last line without its end) */
      if (m3u_len + strlen(buf) < sizeof(m3u))
      {
         memcpy(m3u + m3u_len, buf, strlen(buf));
         m3u_len += strlen(buf);
      }
   }
   if (rnd() % 20 == 0)
   {
      /* a long line (under Windows' PATH_MAX, which the lines are read in) */
      size_t end = m3u_len + 200;
      while (m3u_len < end)
         m3u[m3u_len++] = (char)('a' + rnd() % 26);
      m3u[m3u_len++] = '\n';
   }
}

static void ask_frontend(void)
{
   unsigned n, i;
   char buf[64];
   mix(dc.get_eject_state());
   mix(dc.get_image_index());
   n = dc.get_num_images();
   mix(n);
   for (i = 0; i <= n + 1; i++)
   {
      if (dc.get_image_path != NULL)
      {
         size_t len = rnd() % 3 == 0 ? (size_t)(rnd() % 8) : sizeof(buf);
         memset(buf, 'z', sizeof(buf));
         mix(dc.get_image_path(i, buf, len));
         buf[sizeof(buf) - 1] = '\0';
         mixs(buf);
      }
      if (dc.get_image_label != NULL)
      {
         size_t len = rnd() % 3 == 0 ? (size_t)(rnd() % 8) : sizeof(buf);
         memset(buf, 'z', sizeof(buf));
         mix(dc.get_image_label(i, buf, len));
         buf[sizeof(buf) - 1] = '\0';
         mixs(buf);
      }
   }
}

int main(void)
{
   static const int systems[] = { DC_PLATFORM_DREAMCAST, DC_PLATFORM_NAOMI, DC_PLATFORM_ATOMISWAVE };
   static char label[16];
   int round;

   log_cb = log_fake;
   short_pathname_representation(label, "/a/very/long/name/of/a/disc.gdi", sizeof(label));
   mixs(label);

   for (round = 0; round < 4000; round++)
   {
      int op;
      char *game;

      if (round % 200 == 0 && round != 0)
         printf("round %d %08x\n", round, (unsigned)hash);

      has_ext = rnd() % 4 != 0;
      lr_disk_init_interface(env_fake);
      settings.System = systems[rnd() % 3];
      game_loaded = false;
      present = false;
      swap_fails = false;
      disc_tray_open = false;
      lr_disk_clear();
      settings.imgread.LoadDefaultImage = false;
      settings.imgread.DefaultImage[0] = '\0';

      /* Before the game: the frontend may say which disc it had in */
      if (dc.set_initial_image != NULL && rnd() % 2)
      {
         const char *p = rnd() % 2 ? "/roms/Disc 2.chd" : pieces[rnd() % 3];
         mix(dc.set_initial_image(rnd() % 5, rnd() % 8 == 0 ? "" : p));
      }
      if (rnd() % 5 == 0)
         mix(dc.set_image_index(rnd() % 3));

      /* The content: a playlist, or a disc */
      if (rnd() % 3 != 0)
      {
         bool ok;
         make_m3u();
         m3u_missing = rnd() % 20 == 0;
         ok = lr_disk_read_m3u("/roms/game.m3u", rnd() % 2 ? "/roms/" : "");
         mix(ok);
         if (!ok)
         {
            ask_frontend();
            mix(hash);
            continue;
         }
      }
      game = lr_disk_start("/roms/Game (Disc 1).gdi");
      mixs(game);
      free(game);
      game_loaded = rnd() % 8 != 0;
      mix(lr_disk_count());
      mix(disk_index);
      ask_frontend();

      for (op = 0; op < 30; op++)
      {
         u32 r = rnd() % 100;
         swap_fails = rnd() % 6 == 0;
         if (r < 15)
            mix(dc.set_eject_state(rnd() % 2 != 0));
         else if (r < 30)
            mix(dc.set_image_index(rnd() % (dc.get_num_images() + 3)));
         else if (r < 38)
            mix(dc.add_image_index());
         else if (r < 50)
         {
            struct retro_game_info info;
            unsigned n = dc.get_num_images();
            memset(&info, 0, sizeof(info));
            info.path = rnd() % 6 == 0 ? "" : pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))];
            mix(dc.replace_image_index(rnd() % (n + 2), rnd() % 3 == 0 ? NULL : &info));
         }
         else if (r < 65)
         {
            u8 words[LR_DISK_TRAILER_SIZE];
            lr_disk_trailer_write(words);
            {
               int i;
               for (i = 0; i < LR_DISK_TRAILER_SIZE; i++)
                  mix(words[i]);
            }
            if (rnd() % 2)
            {
               u32 w = rnd() % (dc.get_num_images() + 2);
               memcpy(words + 8, &w, 4);
            }
            if (rnd() % 3 == 0)
               words[12] = (u8)(rnd() % 4);
            if (rnd() % 10 == 0)
               words[rnd() % 8] ^= 1;
            if (rnd() % 4 == 0)
               settings.System = systems[rnd() % 3];
            lr_disk_trailer_read(words);
         }
         else if (r < 70)
         {
            game_loaded = !game_loaded;
         }
         else
            ask_frontend();
         mix(disk_index);
         mix(disc_tray_open);
         mix(present);
         mix(settings.imgread.LoadDefaultImage);
         mixs(settings.imgread.DefaultImage);
      }
   }
   lr_disk_clear();
   lr_disk_init_interface(env_fake);
   printf("done %08x\n", (unsigned)hash);
   return 0;
}

/* See parse_arena_gl.h. C89. */
#include <string.h>
#include <stdlib.h>
#include <features/features_cpu.h>
#include "parse_arena_gl.h"

#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif

#define STORAGE (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)

/* A buffer of @bytes, its mapping to @map */
static GLuint make_buffer(size_t bytes, void **map)
{
   GLuint b = 0;
   glGenBuffers(1, &b);
   glBindBuffer(GL_COPY_WRITE_BUFFER, b);
   glBufferStorage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)bytes, NULL, STORAGE | GL_CLIENT_STORAGE_BIT);
   *map = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)bytes, STORAGE);
   glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
   return b;
}

/* Whether @bytes at @map read at about the speed of memory of malloc()'s:
 * not where the driver gave write-combined memory */
static int reads_fast(unsigned char *map, size_t bytes)
{
   unsigned char *plain = (unsigned char *)malloc(bytes);
   volatile unsigned char sink = 0;
   retro_time_t t0, t1, t2;
   size_t i;
   int r;

   if (plain == NULL)
      return 0;
   memset(plain, 1, bytes);
   memset(map, 1, bytes);
   t0 = cpu_features_get_time_usec();
   for (r = 0; r < 4; r++)
      for (i = 0; i < bytes; i += 64)
         sink ^= *(volatile unsigned char *)(plain + i);
   t1 = cpu_features_get_time_usec();
   for (r = 0; r < 4; r++)
      for (i = 0; i < bytes; i += 64)
         sink ^= *(volatile unsigned char *)(map + i);
   t2 = cpu_features_get_time_usec();
   free(plain);
   (void)sink;
   return (t2 - t1) <= 4 * (t1 - t0) + 200;
}

int parse_arena_gl_init(parse_arena_gl_t *a, const size_t *room, unsigned slots)
{
   const size_t probe = room[PARSE_ARENA_VERTS] < ((size_t)1 << 20) ? room[PARSE_ARENA_VERTS] : (size_t)1 << 20;
   unsigned i, p;

   memset(a, 0, sizeof(*a));
   if (parse_arena_layout(&a->arena, room, slots, 1) == 0)
      return 0;
   for (i = 0; i < slots; i++)
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
      {
         void *m = NULL;
         a->buffer[i][p] = make_buffer(a->arena.space[p], &m);
         if (m == NULL)
         {
            parse_arena_gl_term(a);
            return 0;
         }
         parse_arena_bind_slot(&a->arena, i, p, m);
      }
   if (!reads_fast(a->arena.part_mem[0][PARSE_ARENA_VERTS], probe))
   {
      parse_arena_gl_term(a);
      return 0;
   }
   a->ready = 1;
   return 1;
}

void parse_arena_gl_term(parse_arena_gl_t *a)
{
   unsigned i, p;

   if (a->ready)
      parse_arena_detach(&a->arena);
   for (i = 0; i < PARSE_ARENA_SLOTS; i++)
   {
      if (a->fence[i] != NULL)
         glDeleteSync(a->fence[i]);
      /* (deleting a buffer unmaps it) */
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
         if (a->buffer[i][p] != 0)
            glDeleteBuffers(1, &a->buffer[i][p]);
   }
   memset(a, 0, sizeof(*a));
}

/* Whether the frame last drawn from slot @s is done on the card, asked
 * without waiting */
static int slot_free(parse_arena_gl_t *a, unsigned s)
{
   GLenum st;

   if (a->fence[s] == NULL)
      return 1;
   st = glClientWaitSync(a->fence[s], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
   if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
      return 0;
   glDeleteSync(a->fence[s]);
   a->fence[s] = NULL;
   return 1;
}

int parse_arena_gl_attach(parse_arena_gl_t *a, ta_list_t *const *lists, size_t vert_head)
{
   unsigned i;

   if (!a->ready || a->arena.slot >= 0)
      return 0;
   for (i = 0; i < a->arena.slices; i++)
   {
      const unsigned s = (a->next + i) % a->arena.slices;
      if (!slot_free(a, s))
         continue;
      if (!parse_arena_attach(&a->arena, s, lists, vert_head))
         return 0;
      a->next = (s + 1) % a->arena.slices;
      a->sorted = 0;
      return 1;
   }
   return 0;
}

void parse_arena_gl_done(parse_arena_gl_t *a)
{
   const int s = a->arena.slot;

   if (!a->ready || s < 0)
      return;
   a->fence[s] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
   parse_arena_detach(&a->arena);
}

void parse_arena_gl_cancel(parse_arena_gl_t *a)
{
   if (a->ready)
      parse_arena_detach(&a->arena);
}

GLuint parse_arena_gl_buffer(const parse_arena_gl_t *a, unsigned part)
{
   return a->ready && a->arena.slot >= 0 && part < PARSE_ARENA_PARTS ? a->buffer[a->arena.slot][part] : 0;
}

void *parse_arena_gl_sorted(parse_arena_gl_t *a, size_t bytes, size_t *offset)
{
   const size_t at = a->arena.room[PARSE_ARENA_IDX] + a->sorted;

   if (!a->ready || a->arena.slot < 0 || bytes > a->arena.sort_room - a->sorted)
      return NULL;
   a->sorted += bytes;
   *offset = at;
   return a->arena.part_mem[a->arena.slot][PARSE_ARENA_IDX] + at;
}

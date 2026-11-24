/* What newer C libraries declare that the ones the core is checked with
 * here do not yet: a file of the core that names a function of its own
 * the same way does not build there (Android's NDK, 37 and up, has C23's
 * free_sized()). Each is declared as those libraries have it, so a static
 * function of that name is an error here too. (c89_check.sh) */
#ifndef C89_NEWER_LIBC_H
#define C89_NEWER_LIBC_H
/* (no header: what it would define first, _WIN32_WINNT on MinGW, the
 * file may define itself) */
#define C89_SIZE_T __SIZE_TYPE__
void free_sized(void *ptr, C89_SIZE_T size);
void free_aligned_sized(void *ptr, C89_SIZE_T alignment, C89_SIZE_T size);
C89_SIZE_T memalignment(const void *p);
void *memset_explicit(void *s, int c, C89_SIZE_T n);
char *strdup(const char *s);
char *strndup(const char *s, C89_SIZE_T n);
void *memccpy(void *dest, const void *src, int c, C89_SIZE_T n);
#undef C89_SIZE_T
#endif

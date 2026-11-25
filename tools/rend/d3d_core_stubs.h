/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* What the Direct3D renderers' tests on a device take from the core: its
 * log, where it writes, and libretro-common's files (with stdio). One
 * program's, included once; C and C++ alike. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "paths.h"
#include <streams/file_stream.h>
#include "log/Log.h"

/* where the files are written: the directory, ending with its separator */
static char dataDir[1024];

/* the log's warnings and errors */
static void RETRO_CALLCONV test_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	printf("  log: ");
	vprintf(fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = test_log;

size_t writable_data_path(char *out, size_t size, const char *name)
{
	const size_t d = strlen(dataDir), l = d + strlen(name);
	if (size)
	{
		size_t n = l < size - 1 ? l : size - 1;
		size_t a = d < n ? d : n;
		memcpy(out, dataDir, a);
		memcpy(out + a, name, n - a);
		out[n] = '\0';
	}
	return l;
}

struct RFILE { FILE *f; };

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	FILE *f;
	RFILE *r;
	(void)hints;
	f = fopen(path, mode == RETRO_VFS_FILE_ACCESS_WRITE ? "wb" : "rb");
	if (f == NULL)
		return NULL;
	r = (RFILE *)malloc(sizeof(*r));
	if (r == NULL)
	{
		fclose(f);
		return NULL;
	}
	r->f = f;
	return r;
}

int64_t filestream_write(RFILE *r, const void *data, int64_t len)
{
	return (int64_t)fwrite(data, 1, (size_t)len, r->f);
}

int filestream_close(RFILE *r)
{
	fclose(r->f);
	free(r);
	return 0;
}

int filestream_delete(const char *path)
{
	return remove(path);
}

int filestream_rename(const char *from, const char *to)
{
	return rename(from, to);
}

int64_t filestream_read_file(const char *path, void **buf, int64_t *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *p;
	*buf = NULL;
	*len = 0;
	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc((size_t)n + 1);
	if (p == NULL || fread(p, 1, (size_t)n, f) != (size_t)n)
	{
		free(p);
		fclose(f);
		return 0;
	}
	fclose(f);
	*buf = p;
	*len = n;
	return 1;
}

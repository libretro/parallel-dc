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
 * program's, included once. */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <streams/file_stream.h>
#include "log/Log.h"

static std::string dataDir;

void GenericLog(LogTypes::LOG_LEVELS level, LogTypes::LOG_TYPE type, const char *file, int line, const char *fmt, ...)
{
	(void)type; (void)file; (void)line;
	va_list ap;
	va_start(ap, fmt);
	if (level <= LogTypes::LWARNING)
	{
		printf("  log: ");
		vprintf(fmt, ap);
		printf("\n");
	}
	va_end(ap);
}

std::string get_writable_data_path(const std::string& name)
{
	return dataDir + name;
}

struct RFILE { FILE *f; };

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)hints;
	FILE *f = fopen(path, mode == RETRO_VFS_FILE_ACCESS_WRITE ? "wb" : "rb");
	if (f == nullptr)
		return nullptr;
	RFILE *r = new RFILE;
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
	delete r;
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
	*buf = nullptr;
	*len = 0;
	if (f == nullptr)
		return 0;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc((size_t)n + 1);
	if (p == nullptr || fread(p, 1, (size_t)n, f) != (size_t)n)
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

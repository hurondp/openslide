/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2015 Benjamin Gilbert
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

#if !defined(HAVE_FSEEKO) && defined(_WIN32)
#define fseeko _fseeki64
#endif
#if !defined(HAVE_FTELLO) && defined(_WIN32)
#define ftello _ftelli64
#endif

struct _openslide_file {
  FILE *fp;
  char *path;
};

struct _openslide_dir {
  GDir *dir;
  char *path;
};

static void wrap_fclose(FILE *fp) {
  fclose(fp);  // ci-allow
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, wrap_fclose)

static void io_error(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void io_error(GError **err, const char *fmt, ...) {
  int my_errno = errno;
  va_list ap;

  va_start(ap, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, ap);
  g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(my_errno),
              "%s: %s", msg, g_strerror(my_errno));
  va_end(ap);
}

struct _openslide_file *_openslide_fopen(const char *path, GError **err) {
  g_autoptr(FILE) f = NULL;

#ifdef _WIN32
  g_autofree wchar_t *path16 =
    (wchar_t *) g_utf8_to_utf16(path, -1, NULL, NULL, err);
  if (path16 == NULL) {
    g_prefix_error(err, "Couldn't open %s: ", path);
    return NULL;
  }
  f = _wfopen(path16, L"rb" FOPEN_CLOEXEC_FLAG);
  if (f == NULL) {
    io_error(err, "Couldn't open %s", path);
    return NULL;
  }
#else
  f = fopen(path, "rb" FOPEN_CLOEXEC_FLAG);  // ci-allow
  if (f == NULL) {
    io_error(err, "Couldn't open %s", path);
    return NULL;
  }
  /* Unnecessary if FOPEN_CLOEXEC_FLAG is non-empty, but compile-checked */
  if (!FOPEN_CLOEXEC_FLAG[0]) {
    int fd = fileno(f);
    if (fd == -1) {
      io_error(err, "Couldn't fileno() %s", path);
      return NULL;
    }
    long flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
      io_error(err, "Couldn't F_GETFD %s", path);
      return NULL;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
      io_error(err, "Couldn't F_SETFD %s", path);
      return NULL;
    }
  }
#endif

  struct _openslide_file *file = g_new0(struct _openslide_file, 1);
  file->fp = g_steal_pointer(&f);
  file->path = g_strdup(path);
  return file;
}

// returns 0/NULL on EOF and 0/non-NULL on I/O error
size_t _openslide_fread(struct _openslide_file *file, void *buf, size_t size,
                        GError **err) {
  char *bufp = buf;
  size_t total = 0;
  while (total < size) {
    size_t count = fread(bufp + total, 1, size - total, file->fp);  // ci-allow
    if (count == 0) {
      break;
    }
    total += count;
  }
  if (total == 0 && ferror(file->fp)) {
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_IO,
                "I/O error reading file %s", file->path);
  }
  return total;
}

bool _openslide_fread_exact(struct _openslide_file *file,
                            void *buf, size_t size, GError **err) {
  GError *tmp_err = NULL;
  size_t count = _openslide_fread(file, buf, size, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  } else if (count < size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Short read of file %s: %"PRIu64" < %"PRIu64,
                file->path, (uint64_t) count, (uint64_t) size);
    return false;
  }
  return true;
}

bool _openslide_fseek(struct _openslide_file *file, int64_t offset, int whence,
                      GError **err) {
  if (fseeko(file->fp, offset, whence)) {  // ci-allow
    io_error(err, "Couldn't seek file %s", file->path);
    return false;
  }
  return true;
}

int64_t _openslide_ftell(struct _openslide_file *file, GError **err) {
  int64_t ret = ftello(file->fp);  // ci-allow
  if (ret == -1) {
    io_error(err, "Couldn't get offset of %s", file->path);
  }
  return ret;
}

int64_t _openslide_fsize(struct _openslide_file *file, GError **err) {
  int64_t orig = _openslide_ftell(file, err);
  if (orig == -1) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  if (!_openslide_fseek(file, 0, SEEK_END, err)) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  int64_t ret = _openslide_ftell(file, err);
  if (ret == -1) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  if (!_openslide_fseek(file, orig, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  return ret;
}

void _openslide_fclose(struct _openslide_file *file) {
  fclose(file->fp);  // ci-allow
  g_free(file->path);
  g_free(file);
}

bool _openslide_fexists(const char *path, GError **err G_GNUC_UNUSED) {
  return g_file_test(path, G_FILE_TEST_EXISTS);  // ci-allow
}

struct _openslide_dir *_openslide_dir_open(const char *dirname, GError **err) {
  g_autoptr(_openslide_dir) d = g_new0(struct _openslide_dir, 1);
  d->dir = g_dir_open(dirname, 0, err);
  if (!d->dir) {
    return NULL;
  }
  d->path = g_strdup(dirname);
  return g_steal_pointer(&d);
}

const char *_openslide_dir_next(struct _openslide_dir *d, GError **err) {
  errno = 0;
  const char *ret = g_dir_read_name(d->dir);
  if (!ret && errno) {
    io_error(err, "Reading directory %s", d->path);
  }
  return ret;
}

void _openslide_dir_close(struct _openslide_dir *d) {
  if (d->dir) {
    g_dir_close(d->dir);
  }
  g_free(d->path);
  g_free(d);
}

/* Minimal POSIX <unistd.h> for MuPDF under vbcc - see compat/README.md. */
#ifndef WPDF_UNISTD_H
#define WPDF_UNISTD_H

#include <sys/types.h>

/* vbcc's stdio has no 64-bit offsets and no file descriptors: files are
   < 2 GB here anyway, and the one fileno() user only truncates. */
#define fseeko fseek
#define ftello ftell
#define fileno(f) 0

int unlink(const char *path);
int ftruncate(int fd, off_t length);

#endif

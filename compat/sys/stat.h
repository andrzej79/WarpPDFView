/* Minimal POSIX <sys/stat.h> for MuPDF under vbcc - see compat/README.md. */
#ifndef WPDF_SYS_STAT_H
#define WPDF_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat {
  mode_t st_mode;
  off_t st_size;
  time_t st_mtime;
  time_t st_ctime;
};

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#undef S_ISDIR
#undef S_ISREG
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007

int stat(const char *path, struct stat *st);
int mkdir(const char *path, mode_t mode);

#endif

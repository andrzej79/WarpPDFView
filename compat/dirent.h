/* Minimal POSIX <dirent.h> for MuPDF under vbcc - see compat/README.md.
   Implemented on dos.library in compat/compat.c. */
#ifndef WPDF_DIRENT_H
#define WPDF_DIRENT_H

struct dirent {
  char d_name[108];
};

typedef struct WpdfDir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dp);
int closedir(DIR *dp);

#endif

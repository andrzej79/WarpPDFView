/* Minimal POSIX <sys/time.h> for MuPDF under vbcc - see compat/README.md.
   Deliberately not the NDK's struct timeval (devices/timer.h): the two never
   meet in one file. */
#ifndef WPDF_SYS_TIME_H
#define WPDF_SYS_TIME_H

#include <time.h>

struct timeval {
  long tv_sec;
  long tv_usec;
};

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);

#endif

/**
 * The handful of POSIX calls MuPDF makes that vbcc's libc does not have,
 * on dos.library.  See compat/README.md.
 *
 * None of them is on a rendering path: MuPDF uses them for temporary files,
 * directory "documents" and time stamps, which a PDF viewer barely touches.
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "sys/stat.h"
#include "unistd.h"
#include "fcntl.h"
#include "dirent.h"

// Seconds from 1.1.1970 to 1.1.1978, the AmigaDOS epoch.
#define AMIGA_EPOCH_OFFSET 252460800UL

int unlink(const char *path)
{
  return DeleteFile((STRPTR)path) ? 0 : -1;
}

int ftruncate(int fd, off_t length)
{
  (void)fd;
  (void)length;
  return -1;
}

int open(const char *path, int flags, ...)
{
  (void)path;
  (void)flags;
  return -1;
}

int stat(const char *path, struct stat *st)
{
  BPTR lock;
  struct FileInfoBlock *fib;
  int ret = -1;

  lock = Lock((STRPTR)path, ACCESS_READ);
  if(lock == 0) {
    return -1;
  }
  fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
  if(fib != NULL) {
    if(Examine(lock, fib)) {
      st->st_mode = (fib->fib_DirEntryType > 0) ? S_IFDIR : S_IFREG;
      st->st_size = fib->fib_Size;
      st->st_mtime = (time_t)(AMIGA_EPOCH_OFFSET + fib->fib_Date.ds_Days * 86400UL +
                              fib->fib_Date.ds_Minute * 60UL + fib->fib_Date.ds_Tick / TICKS_PER_SECOND);
      st->st_ctime = st->st_mtime;
      ret = 0;
    }
    FreeDosObject(DOS_FIB, fib);
  }
  UnLock(lock);
  return ret;
}

int mkdir(const char *path, mode_t mode)
{
  BPTR lock;

  (void)mode;
  lock = CreateDir((STRPTR)path);
  if(lock == 0) {
    return -1;
  }
  UnLock(lock);
  return 0;
}

/* compat/sys/time.h cannot be included here: the NDK's devices/timer.h,
   which dos.h pulls in, has its own struct timeval.  The same layout under
   another tag - C links by name, not by type. */
struct wpdfTimeval {
  long tv_sec;
  long tv_usec;
};

struct wpdfTimezone {
  int tz_minuteswest;
  int tz_dsttime;
};

int gettimeofday(struct wpdfTimeval *tv, struct wpdfTimezone *tz)
{
  struct DateStamp ds;

  DateStamp(&ds);
  if(tv != NULL) {
    tv->tv_sec = (long)(AMIGA_EPOCH_OFFSET + ds.ds_Days * 86400UL + ds.ds_Minute * 60UL + ds.ds_Tick / TICKS_PER_SECOND);
    tv->tv_usec = (long)((ds.ds_Tick % TICKS_PER_SECOND) * (1000000UL / TICKS_PER_SECOND));
  }
  if(tz != NULL) {
    tz->tz_minuteswest = 0;
    tz->tz_dsttime = 0;
  }
  return 0;
}

struct WpdfDir {
  BPTR lock;
  struct FileInfoBlock *fib;
  struct dirent ent;
};

DIR *opendir(const char *path)
{
  DIR *dp = (DIR *)calloc(1, sizeof(DIR));

  if(dp == NULL) {
    return NULL;
  }
  dp->lock = Lock((STRPTR)path, ACCESS_READ);
  dp->fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
  if(dp->lock == 0 || dp->fib == NULL || !Examine(dp->lock, dp->fib) || dp->fib->fib_DirEntryType <= 0) {
    closedir(dp);
    return NULL;
  }
  return dp;
}

struct dirent *readdir(DIR *dp)
{
  if(!ExNext(dp->lock, dp->fib)) {
    return NULL;
  }
  strncpy(dp->ent.d_name, (const char *)dp->fib->fib_FileName, sizeof(dp->ent.d_name) - 1);
  dp->ent.d_name[sizeof(dp->ent.d_name) - 1] = 0;
  return &dp->ent;
}

int closedir(DIR *dp)
{
  if(dp == NULL) {
    return -1;
  }
  if(dp->fib != NULL) {
    FreeDosObject(DOS_FIB, dp->fib);
  }
  if(dp->lock != 0) {
    UnLock(dp->lock);
  }
  free(dp);
  return 0;
}

size_t strnlen(const char *s, size_t max)
{
  size_t n = 0;

  while(n < max && s[n] != 0) {
    n++;
  }
  return n;
}

// random()/srandom(): MuPDF's only use is seeding and drawing IDs.
static unsigned long randState = 1;

void srandom(unsigned int seed)
{
  randState = (seed != 0) ? seed : 1;
}

long random(void)
{
  // xorshift32, 31 bits out like the BSD original.
  randState ^= randState << 13;
  randState ^= randState >> 17;
  randState ^= randState << 5;
  return (long)(randState & 0x7FFFFFFFUL);
}

/* Not a cryptographic source - there is none on AmigaOS.  MuPDF only draws
   file IDs and AES IVs for files it writes; a viewer writes none. */
int getentropy(void *buf, size_t len)
{
  struct DateStamp ds;
  unsigned char *p = (unsigned char *)buf;
  size_t i;

  DateStamp(&ds);
  srandom((unsigned int)(ds.ds_Days * 1440UL * 3000UL + ds.ds_Minute * 3000UL + ds.ds_Tick) ^
          (unsigned int)(unsigned long)buf);
  for(i = 0; i < len; i++) {
    p[i] = (unsigned char)(random() >> 11);
  }
  return 0;
}

// UTC struct tm -> seconds since 1970, no time zone (days_from_civil).
time_t timegm(struct tm *tm)
{
  long y = tm->tm_year + 1900L;
  long m = tm->tm_mon + 1L;
  long era, yoe, doy, doe, days;

  if(m <= 2) {
    y--;
  }
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = era * 146097L + doe - 719468L;
  return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

#ifndef WORKER_H
#define WORKER_H

/**
 * The render worker: a DOS process, one priority below the UI, that is the
 * only code touching MuPDF.  The UI queues jobs and reads results; nothing it
 * does waits for a page.
 *
 * On a single 68060 a second task makes nothing faster.  What it buys is that
 * the window keeps answering while a page draws, that idle time goes into the
 * next pages, and that a page nobody wants any more stops drawing: MuPDF
 * checks an fz_cookie's abort flag as it interprets and as it draws, and
 * workerRetain() sets it.
 *
 * Rendered pages live in a cache in fast RAM, least recently used first,
 * within the budget workerSetCache() gives.  The UI pins what it shows
 * (workerLookup), and the worker only ever frees an unpinned page, so a page
 * cannot go between the result saying it is there and the UI taking it.  Both
 * happen under one semaphore.
 *
 * Every job and result carries the document generation workerOpen() returned:
 * whatever still arrives for a document that has been replaced is ignored.
 */

#include <exec/types.h>
#include <exec/lists.h>

typedef struct {
  UBYTE *pixels;     // RGB, 3 bytes a pixel
  LONG stride;       // bytes per row
  LONG w, h;
} PdfImage;

typedef struct CacheEntry CacheEntry;

// Result types
enum {
  WR_OPENED = 1,       // pageCount, text = title, w/h = first page size
  WR_OPEN_FAILED,      // text = the error
  WR_RENDERED,         // page at scaleKey/aa is in the cache: workerLookup() it
  WR_RENDER_FAILED,    // text = the error
  WR_SIZES,            // more page sizes are known (workerPageSize)
  WR_SEARCH_HIT,       // page, hitCount, hits
  WR_SEARCH_PROGRESS,  // page being searched
  WR_SEARCH_DONE,      // no (more) hits anywhere
  WR_OUTLINE           // outline, outlineCount: the bookmarks (possibly none)
};

// One bookmark, the outline flattened in document order.
typedef struct {
  LONG page;           // 0-based, -1: points nowhere in this document
  float y;             // where on the page, in points from its top
  WORD level;          // 0 at the top of the tree
  char title[94];      // Latin-1, the PDF's UTF-8 converted
} WorkerBookmark;

#define WORKER_MAX_HITS 64

typedef struct {
  struct MinNode node;   // the worker queues results through this: keep it first
  int type;
  ULONG docGen;
  LONG page;
  ULONG scaleKey;      // see workerScaleKey()
  int aa;
  ULONG listMs, drawMs;
  LONG pageCount;
  float w, h;
  ULONG searchGen;
  int searchDir;
  LONG hitCount;
  // x0, y0, x1, y1 per hit, in points from the page's top left
  float hits[WORKER_MAX_HITS * 4];
  char text[256];
  /* WR_OUTLINE: AllocVec()ed.  Take it by setting outline to NULL; else
     workerFreeResult() frees it with the result. */
  WorkerBookmark *outline;
  LONG outlineCount;
} WorkerResult;

// Priorities: lower first
#define WPRIO_VISIBLE  1
#define WPRIO_SEARCH   2
#define WPRIO_PREFETCH 3

/* Start the worker.  *sigMask gets the signal the UI should wait for; on it,
   drain workerGetResult(). */
BOOL workerStart(ULONG cacheBytes, ULONG *sigMask);
void workerStop(void);
void workerSetCache(ULONG bytes);

/* The scale as the cache and the jobs know it: floats are not compared for
   equality anywhere. */
ULONG workerScaleKey(float scale);

/* Open a document.  Everything queued for the old one is dropped and the page
   running is aborted.  The UI must have unpinned all its pages first. */
ULONG workerOpen(const char *path);

/* Queue a page for rendering (nothing happens if it is queued already, bar a
   raised priority). */
void workerRender(LONG page, float scale, int aa, int prio);

typedef struct {
  LONG page;
  ULONG scaleKey;
  int aa;
} RenderKey;

/* Keep only these render jobs: the rest are dropped from the queue, and the
   one running is aborted if it is not among them. */
void workerRetain(const RenderKey *keys, int n);

/* Search for needle from page on, dir +1 or -1, wrapping round once.  A new
   search replaces any running one. */
ULONG workerSearch(const char *needle, LONG page, int dir);
void workerCancelSearch(void);

WorkerResult *workerGetResult(void);
void workerFreeResult(WorkerResult *r);

/* The cached page, pinned; NULL if it is not (or no longer) in the cache. */
CacheEntry *workerLookup(LONG page, ULONG scaleKey, int aa);
void workerUnpin(CacheEntry *e);
const PdfImage *workerImage(CacheEntry *e);

// Diagnostics: renders abandoned half way (aborted by workerRetain or a new document).
ULONG workerAborts(void);

/* A page's size in points, rotation applied.  FALSE until the worker has
   measured it (the first page is measured on opening). */
BOOL workerPageSize(LONG page, float *w, float *h);

#endif

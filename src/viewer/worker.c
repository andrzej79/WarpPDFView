/**
 * The render worker - see worker.h.
 *
 * Shared between the two tasks, all under `lock`: the job queue, the result
 * queue, the page cache (entries, pins, the byte count), the page sizes and
 * the job running with its cookie.  Everything MuPDF - the context, the
 * document, the display lists - belongs to the worker alone, as does the heap
 * in wpdfmem.c, which is not reentrant.  Jobs and results are AllocVec()ed:
 * the UI allocates jobs, the worker results, and the other side frees them.
 */

#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>

#include "mupdf/fitz.h"
#include "wpdfmem.h"
#include "worker.h"

enum { J_OPEN = 1, J_RENDER, J_SIZES, J_SEARCH, J_OUTLINE };

#define WPRIO_OPEN    0
#define WPRIO_OUTLINE 2    // after the first page, before anything ahead
#define WPRIO_SIZES   9
#define OUTLINE_MAX   8000
#define OUTLINE_DEPTH 16
#define NEEDLE_MAX  128

typedef struct Job {
  struct MinNode node;
  int type;
  int prio;
  ULONG docGen;
  LONG page;
  ULONG scaleKey;
  float scale;
  int aa;
  int dir;
  ULONG searchGen;
  LONG visited;          // search: pages looked at so far
  char text[512];        // open: the path; search: the needle
} Job;

struct CacheEntry {
  struct MinNode node;
  ULONG docGen;
  LONG page;
  ULONG scaleKey;
  int aa;
  fz_pixmap *pix;
  PdfImage img;
  ULONG bytes;
  LONG pins;
  ULONG lastUse;
};

// ---- shared (under lock) ----------------------------------------------------------
static struct SignalSemaphore lock;
static struct MinList jobs, results, cache;
static Job *cur = NULL;
static fz_cookie cookie;
static ULONG budget = 16UL << 20, cacheBytes = 0, useCounter = 0;
static ULONG docGen = 0;                  // the UI's current document
static ULONG aborts = 0;                  // renders abandoned half way
static volatile ULONG searchGen = 0;      // the UI's current search
static LONG pageCount = 0;
static float *sizeW = NULL, *sizeH = NULL;
static UBYTE *sizeKnown = NULL;

// ---- tasks ------------------------------------------------------------------------
static struct Task *mainTask = NULL;
static struct Process *proc = NULL;
static BYTE resultSig = -1;               // the UI's: results are waiting
static BYTE ackSig = -1;                  // the UI's: the worker started / stopped
static BYTE jobSig = -1;                  // the worker's: jobs are waiting
static volatile BOOL quitReq = FALSE;
static volatile BOOL started = FALSE;

// ---- the worker's own ------------------------------------------------------------
struct Device *TimerBase = NULL;
static struct MsgPort *timerPort = NULL;
static struct timerequest *timerReq = NULL;
static ULONG eclockFreq = 1;

static fz_context *ctx = NULL;
static fz_document *doc = NULL;
static ULONG workDocGen = 0;
static int curAA = -1;

#define LISTS 8
static struct {
  LONG page;
  fz_display_list *list;
  fz_rect bounds;
  ULONG lastUse;
} lists[LISTS];
static ULONG listUse = 0;

// ---- helpers ----------------------------------------------------------------------

ULONG workerScaleKey(float scale)
{
  return (ULONG)(scale * 65536.0f + 0.5f);
}

static unsigned long long clockNow(void)
{
  struct EClockVal ev;

  if(TimerBase == NULL) {
    return 0;
  }
  ReadEClock(&ev);
  return ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
}

static ULONG clockMs(unsigned long long t0, unsigned long long t1)
{
  return (ULONG)(((t1 - t0) * 1000ULL) / eclockFreq);
}

static void *memAlloc(void *user, size_t size)
{
  (void)user;
  return wpdfMemAlloc(size);
}

static void *memRealloc(void *user, void *old, size_t size)
{
  (void)user;
  return wpdfMemRealloc(old, size);
}

static void memFree(void *user, void *ptr)
{
  (void)user;
  wpdfMemFree(ptr);
}

static fz_alloc_context allocCtx = { NULL, memAlloc, memRealloc, memFree };

static WorkerResult *newResult(int type, const Job *j)
{
  WorkerResult *r = (WorkerResult *)AllocVec(sizeof(WorkerResult), MEMF_ANY | MEMF_CLEAR);

  if(r != NULL) {
    r->type = type;
    if(j != NULL) {
      r->docGen = j->docGen;
      r->page = j->page;
      r->scaleKey = j->scaleKey;
      r->aa = j->aa;
      r->searchGen = j->searchGen;
      r->searchDir = j->dir;
    }
  }
  return r;
}

static void post(WorkerResult *r)
{
  if(r == NULL) {
    return;
  }
  ObtainSemaphore(&lock);
  AddTail((struct List *)&results, (struct Node *)r);
  ReleaseSemaphore(&lock);
  Signal(mainTask, 1UL << resultSig);
}

static void postError(int type, const Job *j, const char *msg)
{
  WorkerResult *r = newResult(type, j);

  if(r != NULL) {
    strncpy(r->text, msg, sizeof(r->text) - 1);
    post(r);
  }
}

// Queue a job; the caller holds no lock.
static void queue(Job *j)
{
  ObtainSemaphore(&lock);
  AddTail((struct List *)&jobs, (struct Node *)j);
  ReleaseSemaphore(&lock);
  if(proc != NULL && jobSig >= 0) {
    Signal((struct Task *)proc, 1UL << jobSig);
  }
}

// Under lock: is anything queued that should run before a job of prio?
static BOOL morePressing(int prio)
{
  struct MinNode *n;

  for(n = jobs.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
    if(((Job *)n)->prio < prio) {
      return TRUE;
    }
  }
  return FALSE;
}

// ---- the page cache (worker side) ----------------------------------------------------

// Under lock.
static CacheEntry *findEntry(ULONG gen, LONG page, ULONG scaleKey, int aa)
{
  struct MinNode *n;

  for(n = cache.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
    CacheEntry *e = (CacheEntry *)n;
    if(e->docGen == gen && e->page == page && e->scaleKey == scaleKey && e->aa == aa) {
      return e;
    }
  }
  return NULL;
}

/* Under lock: free unpinned pages, oldest first, until need more bytes fit
   the budget (or nothing unpinned is left). */
static void evict(ULONG need)
{
  while(cacheBytes + need > budget) {
    struct MinNode *n;
    CacheEntry *victim = NULL;
    for(n = cache.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
      CacheEntry *e = (CacheEntry *)n;
      if(e->pins == 0 && (victim == NULL || e->lastUse < victim->lastUse)) {
        victim = e;
      }
    }
    if(victim == NULL) {
      return;
    }
    Remove((struct Node *)victim);
    cacheBytes -= victim->bytes;
    fz_drop_pixmap(ctx, victim->pix);
    FreeVec(victim);
  }
}

// Under lock: every unpinned page (a new document is coming).
static void dropUnpinned(void)
{
  struct MinNode *n, *next;

  for(n = cache.mlh_Head; (next = n->mln_Succ) != NULL; n = next) {
    CacheEntry *e = (CacheEntry *)n;
    if(e->pins == 0) {
      Remove((struct Node *)e);
      cacheBytes -= e->bytes;
      fz_drop_pixmap(ctx, e->pix);
      FreeVec(e);
    }
  }
}

// ---- display lists -------------------------------------------------------------------

static void dropLists(void)
{
  int i;

  for(i = 0; i < LISTS; i++) {
    fz_drop_display_list(ctx, lists[i].list);
    lists[i].list = NULL;
    lists[i].page = -1;
  }
}

static void noteSize(LONG page, fz_rect b)
{
  ObtainSemaphore(&lock);
  if(sizeKnown != NULL && page >= 0 && page < pageCount) {
    sizeW[page] = b.x1 - b.x0;
    sizeH[page] = b.y1 - b.y0;
    sizeKnown[page] = 1;
  }
  ReleaseSemaphore(&lock);
}

/* The page's display list, from the cache or built with the cookie (so an
   abort stops the interpretation too).  NULL: aborted, or an error in err. */
static fz_display_list *getList(LONG page, fz_rect *bounds, ULONG *ms, char *err, ULONG errLen)
{
  fz_page *pg = NULL;
  fz_display_list *list = NULL;
  fz_device *dev = NULL;
  fz_rect b;
  unsigned long long t0;
  int i, slot;

  *ms = 0;
  for(i = 0; i < LISTS; i++) {
    if(lists[i].list != NULL && lists[i].page == page) {
      lists[i].lastUse = ++listUse;
      *bounds = lists[i].bounds;
      return lists[i].list;
    }
  }
  t0 = clockNow();
  fz_var(pg);
  fz_var(list);
  fz_var(dev);
  fz_try(ctx)
  {
    pg = fz_load_page(ctx, doc, (int)page);
    b = fz_bound_page(ctx, pg);
    list = fz_new_display_list(ctx, b);
    dev = fz_new_list_device(ctx, list);
    fz_run_page(ctx, pg, dev, fz_identity, &cookie);
    fz_close_device(ctx, dev);
  }
  fz_always(ctx)
  {
    fz_drop_device(ctx, dev);
    fz_drop_page(ctx, pg);
  }
  fz_catch(ctx)
  {
    fz_drop_display_list(ctx, list);
    snprintf(err, errLen, "Page %ld: %s", (long)(page + 1), fz_caught_message(ctx));
    return NULL;
  }
  if(cookie.abort) {
    fz_drop_display_list(ctx, list);
    return NULL;
  }
  *ms = clockMs(t0, clockNow());
  noteSize(page, b);
  // An empty slot, else the least recently used.
  slot = -1;
  for(i = 0; i < LISTS; i++) {
    if(lists[i].list == NULL) {
      slot = i;
      break;
    }
  }
  if(slot < 0) {
    slot = 0;
    for(i = 1; i < LISTS; i++) {
      if(lists[i].lastUse < lists[slot].lastUse) {
        slot = i;
      }
    }
  }
  fz_drop_display_list(ctx, lists[slot].list);
  lists[slot].page = page;
  lists[slot].list = list;
  lists[slot].bounds = b;
  lists[slot].lastUse = ++listUse;
  *bounds = b;
  return list;
}

// ---- jobs --------------------------------------------------------------------------

static void doOpen(Job *j)
{
  fz_document *d = NULL;
  fz_page *pg = NULL;
  fz_rect b;
  int n = 0;
  WorkerResult *r;
  char title[128];

  // The old document goes first: its pages, lists and sizes.
  dropLists();
  ObtainSemaphore(&lock);
  dropUnpinned();
  if(sizeKnown != NULL) {
    FreeVec(sizeW);
    FreeVec(sizeH);
    FreeVec(sizeKnown);
    sizeW = sizeH = NULL;
    sizeKnown = NULL;
  }
  pageCount = 0;
  ReleaseSemaphore(&lock);
  fz_drop_document(ctx, doc);
  doc = NULL;

  fz_var(d);
  fz_var(pg);
  fz_try(ctx)
  {
    d = fz_open_document(ctx, j->text);
    if(fz_needs_password(ctx, d) && !fz_authenticate_password(ctx, d, "")) {
      fz_throw(ctx, FZ_ERROR_ARGUMENT, "the document is password protected (not supported yet)");
    }
    n = fz_count_pages(ctx, d);
    if(n <= 0) {
      fz_throw(ctx, FZ_ERROR_FORMAT, "the document has no pages");
    }
    pg = fz_load_page(ctx, d, 0);
    b = fz_bound_page(ctx, pg);
  }
  fz_always(ctx)
  {
    fz_drop_page(ctx, pg);
  }
  fz_catch(ctx)
  {
    char msg[256];
    fz_drop_document(ctx, d);
    snprintf(msg, sizeof(msg), "%s:\n%s", j->text, fz_caught_message(ctx));
    postError(WR_OPEN_FAILED, j, msg);
    return;
  }
  doc = d;
  workDocGen = j->docGen;

  ObtainSemaphore(&lock);
  sizeW = (float *)AllocVec((ULONG)n * sizeof(float), MEMF_ANY | MEMF_CLEAR);
  sizeH = (float *)AllocVec((ULONG)n * sizeof(float), MEMF_ANY | MEMF_CLEAR);
  sizeKnown = (UBYTE *)AllocVec((ULONG)n, MEMF_ANY | MEMF_CLEAR);
  if(sizeW == NULL || sizeH == NULL || sizeKnown == NULL) {
    FreeVec(sizeW);
    FreeVec(sizeH);
    FreeVec(sizeKnown);
    sizeW = sizeH = NULL;
    sizeKnown = NULL;
  } else {
    pageCount = n;
  }
  ReleaseSemaphore(&lock);
  noteSize(0, b);

  r = newResult(WR_OPENED, j);
  if(r != NULL) {
    r->pageCount = n;
    r->w = b.x1 - b.x0;
    r->h = b.y1 - b.y0;
    if(fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, title, sizeof(title)) > 0) {
      strncpy(r->text, title, sizeof(r->text) - 1);
    }
    post(r);
  }

  // The bookmarks, once the first page is up.
  {
    Job *o = (Job *)AllocVec(sizeof(Job), MEMF_ANY | MEMF_CLEAR);
    if(o != NULL) {
      o->type = J_OUTLINE;
      o->prio = WPRIO_OUTLINE;
      o->docGen = j->docGen;
      queue(o);
    }
  }

  // The other pages' sizes, for the continuous layout, when nothing else is to do.
  if(n > 1) {
    Job *s = (Job *)AllocVec(sizeof(Job), MEMF_ANY | MEMF_CLEAR);
    if(s != NULL) {
      s->type = J_SIZES;
      s->prio = WPRIO_SIZES;
      s->docGen = j->docGen;
      s->page = 1;
      queue(s);
    }
  }
}

static void doRender(Job *j)
{
  fz_display_list *list;
  fz_pixmap *pm = NULL;
  fz_device *dev = NULL;
  fz_rect bounds;
  fz_irect bbox;
  fz_matrix ctm;
  ULONG listMs = 0, drawMs;
  unsigned long long t0;
  char err[256];
  CacheEntry *e;
  WorkerResult *r;
  BOOL have;

  if(doc == NULL || j->docGen != workDocGen) {
    return;
  }
  ObtainSemaphore(&lock);
  have = (findEntry(j->docGen, j->page, j->scaleKey, j->aa) != NULL) ? TRUE : FALSE;
  ReleaseSemaphore(&lock);
  if(have) {
    post(newResult(WR_RENDERED, j));
    return;
  }
  err[0] = 0;
  list = getList(j->page, &bounds, &listMs, err, sizeof(err));
  if(list == NULL) {
    if(err[0] != 0) {
      postError(WR_RENDER_FAILED, j, err);
    } else {
      ObtainSemaphore(&lock);
      aborts++;
      ReleaseSemaphore(&lock);
    }
    return;   // aborted: nobody wants it any more
  }
  if(curAA != j->aa) {
    fz_set_aa_level(ctx, j->aa);
    curAA = j->aa;
  }
  t0 = clockNow();
  fz_var(pm);
  fz_var(dev);
  fz_try(ctx)
  {
    ctm = fz_scale(j->scale, j->scale);
    bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
    pm = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pm, 0xFF);
    dev = fz_new_draw_device(ctx, fz_identity, pm);
    fz_run_display_list(ctx, list, dev, ctm, fz_infinite_rect, &cookie);
    fz_close_device(ctx, dev);
  }
  fz_always(ctx)
  {
    fz_drop_device(ctx, dev);
  }
  fz_catch(ctx)
  {
    fz_drop_pixmap(ctx, pm);
    snprintf(err, sizeof(err), "Page %ld: %s", (long)(j->page + 1), fz_caught_message(ctx));
    postError(WR_RENDER_FAILED, j, err);
    return;
  }
  if(cookie.abort) {
    fz_drop_pixmap(ctx, pm);
    ObtainSemaphore(&lock);
    aborts++;
    ReleaseSemaphore(&lock);
    return;
  }
  drawMs = clockMs(t0, clockNow());

  e = (CacheEntry *)AllocVec(sizeof(CacheEntry), MEMF_ANY | MEMF_CLEAR);
  if(e == NULL) {
    fz_drop_pixmap(ctx, pm);
    postError(WR_RENDER_FAILED, j, "out of memory");
    return;
  }
  e->docGen = j->docGen;
  e->page = j->page;
  e->scaleKey = j->scaleKey;
  e->aa = j->aa;
  e->pix = pm;
  e->img.pixels = fz_pixmap_samples(ctx, pm);
  e->img.stride = (LONG)fz_pixmap_stride(ctx, pm);
  e->img.w = fz_pixmap_width(ctx, pm);
  e->img.h = fz_pixmap_height(ctx, pm);
  e->bytes = (ULONG)e->img.stride * (ULONG)e->img.h;
  ObtainSemaphore(&lock);
  evict(e->bytes);
  e->lastUse = ++useCounter;
  AddTail((struct List *)&cache, (struct Node *)e);
  cacheBytes += e->bytes;
  ReleaseSemaphore(&lock);

  r = newResult(WR_RENDERED, j);
  if(r != NULL) {
    r->listMs = listMs;
    r->drawMs = drawMs;
    post(r);
  }
}

// One page after another, stepping aside whenever something more pressing is queued.
static BOOL requeue(Job *j)
{
  BOOL yield;

  ObtainSemaphore(&lock);
  yield = morePressing(j->prio);
  if(yield) {
    AddTail((struct List *)&jobs, (struct Node *)j);
  }
  ReleaseSemaphore(&lock);
  return yield;
}

static BOOL doSizes(Job *j)
{
  LONG n = 0;

  while(doc != NULL && j->docGen == workDocGen && j->page < pageCount && !quitReq) {
    fz_page *pg = NULL;
    fz_rect b;
    BOOL ok = TRUE;
    BOOL known;

    ObtainSemaphore(&lock);
    known = (sizeKnown != NULL && sizeKnown[j->page]) ? TRUE : FALSE;
    ReleaseSemaphore(&lock);
    if(!known) {
      fz_var(pg);
      fz_try(ctx)
      {
        pg = fz_load_page(ctx, doc, (int)j->page);
        b = fz_bound_page(ctx, pg);
      }
      fz_always(ctx)
      {
        fz_drop_page(ctx, pg);
      }
      fz_catch(ctx)
      {
        ok = FALSE;
      }
      if(ok) {
        noteSize(j->page, b);
      }
    }
    j->page++;
    if(++n % 16 == 0) {
      post(newResult(WR_SIZES, j));
    }
    if(requeue(j)) {
      return TRUE;
    }
  }
  post(newResult(WR_SIZES, j));
  return FALSE;
}

static BOOL doSearch(Job *j)
{
  fz_quad quads[WORKER_MAX_HITS];
  int marks[WORKER_MAX_HITS];

  while(doc != NULL && j->docGen == workDocGen && j->searchGen == searchGen && !quitReq) {
    fz_display_list *list;
    fz_rect bounds;
    ULONG ms;
    char err[256];
    int hits = 0, i;

    if(j->visited >= pageCount) {
      post(newResult(WR_SEARCH_DONE, j));
      return FALSE;
    }
    err[0] = 0;
    list = getList(j->page, &bounds, &ms, err, sizeof(err));
    if(list == NULL && err[0] == 0) {
      // Aborted from outside: only a new search does that - drop this one.
      return FALSE;
    }
    if(list != NULL) {
      fz_try(ctx)
      {
        hits = fz_search_display_list(ctx, list, j->text, marks, quads, WORKER_MAX_HITS);
      }
      fz_catch(ctx)
      {
        hits = 0;
      }
    }
    if(hits > 0) {
      WorkerResult *r = newResult(WR_SEARCH_HIT, j);
      if(r != NULL) {
        r->hitCount = hits;
        for(i = 0; i < hits; i++) {
          fz_rect q = fz_rect_from_quad(quads[i]);
          r->hits[i * 4 + 0] = q.x0 - bounds.x0;
          r->hits[i * 4 + 1] = q.y0 - bounds.y0;
          r->hits[i * 4 + 2] = q.x1 - bounds.x0;
          r->hits[i * 4 + 3] = q.y1 - bounds.y0;
        }
        post(r);
      }
      return FALSE;
    }
    j->visited++;
    j->page += j->dir;
    if(j->page >= pageCount) {
      j->page = 0;
    } else if(j->page < 0) {
      j->page = pageCount - 1;
    }
    if(j->visited % 4 == 0) {
      post(newResult(WR_SEARCH_PROGRESS, j));
    }
    if(requeue(j)) {
      return TRUE;
    }
  }
  return FALSE;
}

// ---- the outline ---------------------------------------------------------------------

/* UTF-8 to Latin-1, which the Amiga's fonts show.  What Latin-1 lacks but
   titles use - dashes, curly quotes, bullets, ellipses - gets its nearest
   form; anything else a '?'.  Control characters become spaces (MUI would
   take an ESC for a formatting code). */
static void toLatin1(const char *in, char *out, int size)
{
  const unsigned char *s = (const unsigned char *)in;
  int o = 0;

  while(*s != 0 && o < size - 4) {
    unsigned long c = *s++;
    if(c >= 0x80) {
      int more = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
      c &= (more == 3) ? 0x07 : (more == 2) ? 0x0F : 0x1F;
      while(more-- > 0 && (*s & 0xC0) == 0x80) {
        c = (c << 6) | (*s++ & 0x3F);
      }
    }
    if(c < 0x20 || c == 0x7F) {
      out[o++] = ' ';
    } else if(c < 0x100) {
      out[o++] = (char)c;
    } else if(c == 0x2013 || c == 0x2014 || c == 0x2212 || c == 0x2010 || c == 0x2011) {
      out[o++] = '-';
    } else if(c == 0x2018 || c == 0x2019 || c == 0x201A) {
      out[o++] = '\'';
    } else if(c == 0x201C || c == 0x201D || c == 0x201E) {
      out[o++] = '"';
    } else if(c == 0x2022 || c == 0x2219) {
      out[o++] = (char)0xB7;
    } else if(c == 0x2026) {
      out[o++] = '.';
      out[o++] = '.';
      out[o++] = '.';
    } else if(c == 0x2122) {
      out[o++] = 'T';
      out[o++] = 'M';
    } else if(c == 0x2264) {
      out[o++] = '<';
      out[o++] = '=';
    } else if(c == 0x2265) {
      out[o++] = '>';
      out[o++] = '=';
    } else if(c == 0x00A0 || c == 0x2009 || c == 0x200A || c == 0x202F) {
      out[o++] = ' ';
    } else {
      out[o++] = '?';
    }
  }
  out[o] = 0;
}

static LONG countOutline(fz_outline *o, int depth)
{
  LONG n = 0;

  for(; o != NULL && n < OUTLINE_MAX; o = o->next) {
    n++;
    if(o->down != NULL && depth + 1 < OUTLINE_DEPTH) {
      n += countOutline(o->down, depth + 1);
    }
  }
  return n;
}

static void flattenOutline(fz_outline *o, int depth, WorkerBookmark *b, LONG *n, LONG max)
{
  for(; o != NULL && *n < max; o = o->next) {
    WorkerBookmark *e = &b[*n];
    fz_location loc = o->page;
    float x = o->x, y = o->y;

    e->level = (WORD)depth;
    toLatin1((o->title != NULL) ? o->title : "", e->title, sizeof(e->title));
    e->page = -1;
    fz_try(ctx)
    {
      if(loc.page < 0 && o->uri != NULL) {
        loc = fz_resolve_link(ctx, doc, o->uri, &x, &y);
      }
      if(loc.page >= 0) {
        e->page = fz_page_number_from_location(ctx, doc, loc);
      }
    }
    fz_catch(ctx)
    {
      e->page = -1;
    }
    e->y = (y > 0.0f) ? y : 0.0f;
    (*n)++;
    if(o->down != NULL && depth + 1 < OUTLINE_DEPTH) {
      flattenOutline(o->down, depth + 1, b, n, max);
    }
  }
}

static void doOutline(Job *j)
{
  fz_outline *outline = NULL;
  WorkerResult *r;
  LONG n = 0, count;

  if(doc == NULL || j->docGen != workDocGen) {
    return;
  }
  fz_var(outline);
  fz_try(ctx)
  {
    outline = fz_load_outline(ctx, doc);
  }
  fz_catch(ctx)
  {
    outline = NULL;   // a broken outline is no outline
  }
  r = newResult(WR_OUTLINE, j);
  if(r == NULL) {
    fz_drop_outline(ctx, outline);
    return;
  }
  count = countOutline(outline, 0);
  if(count > 0) {
    r->outline = (WorkerBookmark *)AllocVec((ULONG)count * sizeof(WorkerBookmark), MEMF_ANY | MEMF_CLEAR);
    if(r->outline != NULL) {
      flattenOutline(outline, 0, r->outline, &n, count);
    }
  }
  r->outlineCount = n;
  fz_drop_outline(ctx, outline);
  post(r);
}

// ---- the process ---------------------------------------------------------------------

/* MuPDF's warnings and errors would go to stderr - the Shell's, or nowhere
   from Workbench - from this second process.  Broken files are common and
   their warnings are no use to a reader. */
static void quiet(void *user, const char *message)
{
  (void)user;
  (void)message;
}

static BOOL workerInit(void)
{
  timerPort = CreateMsgPort();
  if(timerPort != NULL) {
    timerReq = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
  }
  if(timerReq != NULL && OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)timerReq, 0) == 0) {
    struct EClockVal ev;
    TimerBase = timerReq->tr_node.io_Device;
    eclockFreq = ReadEClock(&ev);
  }
  // The store holds fonts and decoded images; the rendered pages have their own budget.
  ctx = fz_new_context(&allocCtx, NULL, 32UL << 20);
  if(ctx == NULL) {
    return FALSE;
  }
  fz_set_warning_callback(ctx, quiet, NULL);
  fz_set_error_callback(ctx, quiet, NULL);
  fz_try(ctx)
  {
    fz_register_document_handlers(ctx);
  }
  fz_catch(ctx)
  {
    return FALSE;
  }
  jobSig = AllocSignal(-1);
  return (jobSig >= 0) ? TRUE : FALSE;
}

static void workerExit(void)
{
  struct MinNode *n;

  dropLists();
  ObtainSemaphore(&lock);
  while((n = (struct MinNode *)RemHead((struct List *)&cache)) != NULL) {
    CacheEntry *e = (CacheEntry *)n;
    fz_drop_pixmap(ctx, e->pix);
    FreeVec(e);
  }
  cacheBytes = 0;
  while((n = (struct MinNode *)RemHead((struct List *)&jobs)) != NULL) {
    FreeVec(n);
  }
  if(sizeKnown != NULL) {
    FreeVec(sizeW);
    FreeVec(sizeH);
    FreeVec(sizeKnown);
    sizeW = sizeH = NULL;
    sizeKnown = NULL;
  }
  pageCount = 0;
  ReleaseSemaphore(&lock);
  if(ctx != NULL) {
    fz_drop_document(ctx, doc);
    doc = NULL;
    fz_drop_context(ctx);
    ctx = NULL;
  }
  wpdfMemCleanup();
  if(TimerBase != NULL) {
    CloseDevice((struct IORequest *)timerReq);
    TimerBase = NULL;
  }
  if(timerReq != NULL) {
    DeleteIORequest((struct IORequest *)timerReq);
    timerReq = NULL;
  }
  if(timerPort != NULL) {
    DeleteMsgPort(timerPort);
    timerPort = NULL;
  }
  if(jobSig >= 0) {
    FreeSignal(jobSig);
    jobSig = -1;
  }
}

static void workerMain(void)
{
  if(!workerInit()) {
    /* Cleaned up before the UI hears of it: once it has, it may exit and
       unload this code. */
    workerExit();
    Forbid();
    started = FALSE;
    Signal(mainTask, 1UL << ackSig);
    return;
  }
  started = TRUE;
  Signal(mainTask, 1UL << ackSig);
  while(!quitReq) {
    Job *j = NULL;
    struct MinNode *n;

    ObtainSemaphore(&lock);
    for(n = jobs.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
      if(j == NULL || ((Job *)n)->prio < j->prio) {
        j = (Job *)n;
      }
    }
    if(j != NULL) {
      Remove((struct Node *)j);
      cur = j;
      memset(&cookie, 0, sizeof(cookie));
    }
    ReleaseSemaphore(&lock);

    if(j == NULL) {
      Wait(1UL << jobSig);
      continue;
    }
    {
      BOOL requeued = FALSE;
      switch(j->type) {
        case J_OPEN:
          doOpen(j);
          break;
        case J_RENDER:
          doRender(j);
          break;
        case J_SIZES:
          requeued = doSizes(j);
          break;
        case J_SEARCH:
          requeued = doSearch(j);
          break;
        case J_OUTLINE:
          doOutline(j);
          break;
      }
      ObtainSemaphore(&lock);
      cur = NULL;
      ReleaseSemaphore(&lock);
      if(!requeued) {
        FreeVec(j);
      }
    }
  }
  workerExit();
  // Signal inside Forbid(): the process is gone before the UI can unload the code.
  Forbid();
  Signal(mainTask, 1UL << ackSig);
}

// ---- the UI's side -------------------------------------------------------------------

BOOL workerStart(ULONG cacheBytes0, ULONG *sigMask)
{
  InitSemaphore(&lock);
  NewList((struct List *)&jobs);
  NewList((struct List *)&results);
  NewList((struct List *)&cache);
  budget = cacheBytes0;
  mainTask = FindTask(NULL);
  resultSig = AllocSignal(-1);
  ackSig = AllocSignal(-1);
  if(resultSig < 0 || ackSig < 0) {
    return FALSE;
  }
  SetSignal(0, 1UL << ackSig);
  quitReq = FALSE;
  started = FALSE;
  /* One below the UI: the window answers at once whatever is drawing, and
     the worker gets the rest of the machine.  MuPDF recurses deeply. */
  proc = CreateNewProcTags(NP_Entry, (ULONG)workerMain, NP_Name, (ULONG) "WarpPDFView render", NP_Priority,
                           (ULONG)-1, NP_StackSize, 262144, TAG_DONE);
  if(proc == NULL) {
    return FALSE;
  }
  Wait(1UL << ackSig);
  if(!started) {
    // It has already cleaned up and gone.
    proc = NULL;
    return FALSE;
  }
  *sigMask = 1UL << resultSig;
  return TRUE;
}

void workerStop(void)
{
  WorkerResult *r;

  if(proc != NULL) {
    ObtainSemaphore(&lock);
    quitReq = TRUE;
    cookie.abort = 1;
    ReleaseSemaphore(&lock);
    Signal((struct Task *)proc, 1UL << jobSig);
    Wait(1UL << ackSig);
    proc = NULL;
  }
  while((r = workerGetResult()) != NULL) {
    workerFreeResult(r);
  }
  if(resultSig >= 0) {
    FreeSignal(resultSig);
    resultSig = -1;
  }
  if(ackSig >= 0) {
    FreeSignal(ackSig);
    ackSig = -1;
  }
}

void workerSetCache(ULONG bytes)
{
  ObtainSemaphore(&lock);
  budget = bytes;
  ReleaseSemaphore(&lock);
}

static Job *newJob(int type, int prio)
{
  Job *j = (Job *)AllocVec(sizeof(Job), MEMF_ANY | MEMF_CLEAR);

  if(j != NULL) {
    j->type = type;
    j->prio = prio;
    j->docGen = docGen;
  }
  return j;
}

ULONG workerOpen(const char *path)
{
  Job *j;
  struct MinNode *n;

  ObtainSemaphore(&lock);
  docGen++;
  searchGen++;
  while((n = (struct MinNode *)RemHead((struct List *)&jobs)) != NULL) {
    FreeVec(n);
  }
  if(cur != NULL) {
    cookie.abort = 1;
  }
  ReleaseSemaphore(&lock);
  j = newJob(J_OPEN, WPRIO_OPEN);
  if(j != NULL) {
    strncpy(j->text, path, sizeof(j->text) - 1);
    queue(j);
  }
  return docGen;
}

void workerRender(LONG page, float scale, int aa, int prio)
{
  ULONG key = workerScaleKey(scale);
  struct MinNode *n;
  Job *j;

  ObtainSemaphore(&lock);
  if(cur != NULL && cur->type == J_RENDER && cur->docGen == docGen && cur->page == page && cur->scaleKey == key &&
     cur->aa == aa) {
    ReleaseSemaphore(&lock);
    return;
  }
  for(n = jobs.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
    j = (Job *)n;
    if(j->type == J_RENDER && j->docGen == docGen && j->page == page && j->scaleKey == key && j->aa == aa) {
      if(prio < j->prio) {
        j->prio = prio;
      }
      ReleaseSemaphore(&lock);
      return;
    }
  }
  ReleaseSemaphore(&lock);
  j = newJob(J_RENDER, prio);
  if(j != NULL) {
    j->page = page;
    j->scale = scale;
    j->scaleKey = key;
    j->aa = aa;
    queue(j);
  }
}

static BOOL wanted(const Job *j, const RenderKey *keys, int n)
{
  int i;

  for(i = 0; i < n; i++) {
    if(keys[i].page == j->page && keys[i].scaleKey == j->scaleKey && keys[i].aa == j->aa) {
      return TRUE;
    }
  }
  return FALSE;
}

void workerRetain(const RenderKey *keys, int n)
{
  struct MinNode *node, *next;

  ObtainSemaphore(&lock);
  for(node = jobs.mlh_Head; (next = node->mln_Succ) != NULL; node = next) {
    Job *j = (Job *)node;
    if(j->type == J_RENDER && !wanted(j, keys, n)) {
      Remove((struct Node *)j);
      FreeVec(j);
    }
  }
  if(cur != NULL && cur->type == J_RENDER && !wanted(cur, keys, n)) {
    cookie.abort = 1;
  }
  ReleaseSemaphore(&lock);
}

ULONG workerSearch(const char *needle, LONG page, int dir)
{
  Job *j;
  ULONG gen;

  ObtainSemaphore(&lock);
  gen = ++searchGen;
  // A search building a list is stopped; its job notices the new generation.
  if(cur != NULL && cur->type == J_SEARCH) {
    cookie.abort = 1;
  }
  ReleaseSemaphore(&lock);
  j = newJob(J_SEARCH, WPRIO_SEARCH);
  if(j != NULL) {
    strncpy(j->text, needle, NEEDLE_MAX);
    j->page = page;
    j->dir = (dir < 0) ? -1 : 1;
    j->searchGen = gen;
    queue(j);
  }
  return gen;
}

void workerCancelSearch(void)
{
  ObtainSemaphore(&lock);
  searchGen++;
  if(cur != NULL && cur->type == J_SEARCH) {
    cookie.abort = 1;
  }
  ReleaseSemaphore(&lock);
}

WorkerResult *workerGetResult(void)
{
  WorkerResult *r;

  ObtainSemaphore(&lock);
  r = (WorkerResult *)RemHead((struct List *)&results);
  ReleaseSemaphore(&lock);
  return r;
}

void workerFreeResult(WorkerResult *r)
{
  if(r != NULL) {
    if(r->outline != NULL) {
      FreeVec(r->outline);
    }
    FreeVec(r);
  }
}

CacheEntry *workerLookup(LONG page, ULONG scaleKey, int aa)
{
  CacheEntry *e;

  ObtainSemaphore(&lock);
  e = findEntry(docGen, page, scaleKey, aa);
  if(e != NULL) {
    e->pins++;
    e->lastUse = ++useCounter;
  }
  ReleaseSemaphore(&lock);
  return e;
}

void workerUnpin(CacheEntry *e)
{
  if(e == NULL) {
    return;
  }
  ObtainSemaphore(&lock);
  if(e->pins > 0) {
    e->pins--;
  }
  ReleaseSemaphore(&lock);
}

const PdfImage *workerImage(CacheEntry *e)
{
  return (e != NULL) ? &e->img : NULL;
}

ULONG workerAborts(void)
{
  ULONG n;

  ObtainSemaphore(&lock);
  n = aborts;
  ReleaseSemaphore(&lock);
  return n;
}

BOOL workerPageSize(LONG page, float *w, float *h)
{
  BOOL known = FALSE;

  ObtainSemaphore(&lock);
  if(sizeKnown != NULL && page >= 0 && page < pageCount && sizeKnown[page]) {
    *w = sizeW[page];
    *h = sizeH[page];
    known = TRUE;
  }
  ReleaseSemaphore(&lock);
  return known;
}

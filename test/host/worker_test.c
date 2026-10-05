/* Host test of the render worker (src/viewer/worker.c) over the Exec shim.
 *
 *   worker_test <big.pdf> <other.pdf> [needle [heavy page of other.pdf]]
 *
 * big.pdf wants a few dozen pages of text; needle is a word on one of them
 * (default "the"); the heavy page is one of other.pdf's that takes a while
 * to draw, for the in-flight abort.  Each check prints PASS or FAIL; the exit code counts the
 * failures. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include "mupdf/fitz.h"
#include "amigashim.h"
#include "worker.h"

static int failures = 0;
static ULONG sigMask;
static ULONG gen;

static void check(int ok, const char *what)
{
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok) {
    failures++;
  }
}

/* Every result that arrives is kept here until a check looks for it. */
#define KEEP 4096
static WorkerResult *kept[KEEP];
static int nkept = 0;

static void collect(void)
{
  WorkerResult *r;

  while((r = workerGetResult()) != NULL) {
    if(nkept < KEEP) {
      kept[nkept++] = r;
    } else {
      workerFreeResult(r);
    }
  }
}

// The first kept result of type (and page, if >= 0) for docGen g; removed. NULL after ms.
static WorkerResult *await(int type, LONG page, ULONG g, int ms)
{
  int waited = 0, i;

  for(;;) {
    collect();
    for(i = 0; i < nkept; i++) {
      WorkerResult *r = kept[i];
      if(r->type == type && r->docGen == g && (page < 0 || r->page == page)) {
        memmove(&kept[i], &kept[i + 1], sizeof(kept[0]) * (size_t)(nkept - i - 1));
        nkept--;
        return r;
      }
    }
    if(waited >= ms) {
      return NULL;
    }
    usleep(2000);
    waited += 2;
  }
}

static void flushKept(void)
{
  collect();
  while(nkept > 0) {
    workerFreeResult(kept[--nkept]);
  }
}

static unsigned long crcOf(const PdfImage *im)
{
  unsigned long c = crc32(0L, Z_NULL, 0);
  LONG y;

  for(y = 0; y < im->h; y++) {
    c = crc32(c, im->pixels + (size_t)y * (size_t)im->stride, (uInt)(im->w * 3));
  }
  return c;
}

// The same page rendered directly, in this thread, for comparison.
static unsigned long referenceCrc(const char *path, int page, float scale, int aa)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  fz_document *doc;
  fz_pixmap *pix;
  PdfImage im;
  unsigned long c;

  fz_register_document_handlers(ctx);
  fz_set_aa_level(ctx, aa);
  doc = fz_open_document(ctx, path);
  pix = fz_new_pixmap_from_page_number(ctx, doc, page, fz_scale(scale, scale), fz_device_rgb(ctx), 0);
  im.pixels = fz_pixmap_samples(ctx, pix);
  im.stride = (LONG)fz_pixmap_stride(ctx, pix);
  im.w = fz_pixmap_width(ctx, pix);
  im.h = fz_pixmap_height(ctx, pix);
  c = crcOf(&im);
  fz_drop_pixmap(ctx, pix);
  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return c;
}

int main(int argc, char **argv)
{
  const char *big, *other, *needle;
  WorkerResult *r;
  CacheEntry *e0, *e;
  LONG pages, p;
  unsigned long crc0;
  int i, cached, progress;
  char buf[160];

  if(argc < 3) {
    fprintf(stderr, "usage: worker_test <big.pdf> <other.pdf> [needle]\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  big = argv[1];
  other = argv[2];
  needle = (argc > 3) ? argv[3] : "the";
  shimInitMainTask();

  check(workerStart(16UL << 20, &sigMask), "worker starts");

  // 1. Open
  gen = workerOpen(big);
  r = await(WR_OPENED, -1, gen, 10000);
  check(r != NULL && r->pageCount > 10, "open: pages counted");
  pages = r ? r->pageCount : 0;
  check(r != NULL && r->w > 100 && r->h > 100, "open: first page size");
  workerFreeResult(r);

  // 1b. The bookmarks: whatever the file has, every target in range
  r = await(WR_OUTLINE, -1, gen, 60000);
  check(r != NULL, "outline: delivered (with or without bookmarks)");
  if(r != NULL) {
    LONG bad = 0, k;
    for(k = 0; k < r->outlineCount; k++) {
      if(r->outline[k].page < -1 || r->outline[k].page >= pages || r->outline[k].level < 0) {
        bad++;
      }
    }
    printf("      %ld bookmarks\n", (long)r->outlineCount);
    for(k = 0; k < r->outlineCount && k < 6; k++) {
      printf("        %*s%s -> page %ld, y %.0f\n", r->outline[k].level * 2, "", r->outline[k].title,
             (long)(r->outline[k].page + 1), r->outline[k].y);
    }
    check(bad == 0, "outline: every bookmark's page and level in range");
  }
  workerFreeResult(r);

  // 2. Render, and the same pixels as a direct render
  workerRender(0, 1.0f, 8, WPRIO_VISIBLE);
  r = await(WR_RENDERED, 0, gen, 10000);
  check(r != NULL, "render: page 1 arrives");
  workerFreeResult(r);
  e0 = workerLookup(0, workerScaleKey(1.0f), 8);
  check(e0 != NULL, "render: page 1 is in the cache");
  crc0 = e0 ? crcOf(workerImage(e0)) : 0;
  check(e0 != NULL && crc0 == referenceCrc(big, 0, 1.0f, 8), "render: pixels identical to a direct render");

  // 3. Asked again: from the cache
  workerRender(0, 1.0f, 8, WPRIO_VISIBLE);
  r = await(WR_RENDERED, 0, gen, 2000);
  check(r != NULL && r->listMs == 0 && r->drawMs == 0, "cache: a second request is answered from the cache");
  workerFreeResult(r);

  /* 4. Abort.  First the page drawn in full, to know how long it takes; then
     at another scale (the display list is cached now: only drawing is left)
     dropped a third of the way through.  Then pages queued behind a busy
     worker and dropped before they start. */
  {
    ULONG full;
    workerRender(1, 5.0f, 8, WPRIO_VISIBLE);
    r = await(WR_RENDERED, 1, gen, 60000);
    full = r ? r->drawMs : 0;
    printf("      page 2 at 5x draws in %lu ms\n", (unsigned long)full);
    workerFreeResult(r);
    workerRender(1, 5.01f, 8, WPRIO_VISIBLE);
    usleep((useconds_t)(full * 1000 / 3));
    workerRetain(NULL, 0);
    r = await(WR_RENDERED, 1, gen, (int)full * 3 + 500);
    check(r == NULL, "abort: a page dropped while drawing is not delivered");
    workerFreeResult(r);
    e = workerLookup(1, workerScaleKey(5.01f), 8);
    check(e == NULL, "abort: nor cached");
    workerUnpin(e);

    // Queued behind a busy worker, then all but one dropped.
    workerRender(2, 5.0f, 8, WPRIO_VISIBLE);
    for(p = 3; p <= 7; p++) {
      workerRender(p, 1.0f, 8, WPRIO_VISIBLE);
    }
    {
      RenderKey k;
      k.page = 5;
      k.scaleKey = workerScaleKey(1.0f);
      k.aa = 8;
      workerRetain(&k, 1);
    }
    r = await(WR_RENDERED, 5, gen, 60000);
    check(r != NULL, "retain: the page kept is delivered");
    workerFreeResult(r);
    usleep(200000);
    collect();
    {
      int others = 0;
      for(i = 0; i < nkept; i++) {
        if(kept[i]->type == WR_RENDERED && kept[i]->page != 5) {
          others++;
        }
      }
      check(others == 0, "retain: the pages dropped from the queue never render");
    }
    flushKept();
  }

  // 5. Budget: 4 MB, page 1 pinned, ten more pages
  workerSetCache(4UL << 20);
  for(p = 1; p <= 10 && p < pages; p++) {
    workerRender(p, 1.0f, 8, WPRIO_VISIBLE);
  }
  for(p = 1; p <= 10 && p < pages; p++) {
    r = await(WR_RENDERED, p, gen, 20000);
    workerFreeResult(r);
  }
  cached = 0;
  for(p = 1; p <= 10 && p < pages; p++) {
    e = workerLookup(p, workerScaleKey(1.0f), 8);
    if(e != NULL) {
      cached++;
      workerUnpin(e);
    }
  }
  snprintf(buf, sizeof(buf), "budget: %d of 10 unpinned pages kept in 4 MB (~1.5 MB each)", cached);
  check(cached >= 1 && cached <= 2, buf);
  check(e0 != NULL && crcOf(workerImage(e0)) == crc0, "budget: the pinned page survives, pixels intact");
  workerUnpin(e0);
  workerSetCache(16UL << 20);

  // 6. Search: a word that is there
  workerSearch(needle, 0, 1);
  r = await(WR_SEARCH_HIT, -1, gen, 60000);
  check(r != NULL && r->hitCount > 0, "search: finds the word");
  if(r != NULL) {
    check(r->hits[2] > r->hits[0] && r->hits[3] > r->hits[1], "search: hit boxes are boxes");
    printf("      '%s' on page %ld, %ld hits\n", needle, (long)(r->page + 1), (long)r->hitCount);
  }
  workerFreeResult(r);
  flushKept();

  // 7. Search for nothing, with a render asked for meanwhile: the render comes first
  workerSearch("zq-no-such-text-qz", 0, 1);
  usleep(5000);
  workerRender(pages - 1, 1.0f, 8, WPRIO_VISIBLE);
  r = await(WR_RENDERED, pages - 1, gen, 20000);
  check(r != NULL, "priority: a render asked for during a search arrives");
  workerFreeResult(r);
  r = await(WR_SEARCH_DONE, -1, gen, 120000);
  check(r != NULL, "search: a missing word ends with DONE after every page");
  workerFreeResult(r);
  collect();
  progress = 0;
  for(i = 0; i < nkept; i++) {
    if(kept[i]->type == WR_SEARCH_PROGRESS) {
      progress++;
    }
  }
  check(progress > 0, "search: reports progress");
  // The render of the last page must have been received before DONE: checked by the order above.
  flushKept();

  // 8. Sizes: all known in the background
  for(i = 0; i < 3000; i++) {
    float w, h;
    if(workerPageSize(pages - 1, &w, &h)) {
      break;
    }
    usleep(10000);
  }
  {
    int known = 0;
    for(p = 0; p < pages; p++) {
      float w, h;
      if(workerPageSize(p, &w, &h) && w > 0 && h > 0) {
        known++;
      }
    }
    snprintf(buf, sizeof(buf), "sizes: %d of %ld pages measured", known, (long)pages);
    check(known == pages, buf);
  }

  // 9. A new document while pages of the old one render
  for(p = 0; p < 6 && p < pages; p++) {
    workerRender(p, 2.0f, 8, WPRIO_VISIBLE);
  }
  {
    ULONG oldGen = gen;
    gen = workerOpen(other);
    r = await(WR_OPENED, -1, gen, 10000);
    check(r != NULL, "reopen: the new document opens");
    workerFreeResult(r);
    usleep(200000);
    collect();
    for(i = 0; i < nkept; i++) {
      if(kept[i]->type == WR_RENDERED && kept[i]->docGen == gen) {
        break;
      }
    }
    check(i == nkept, "reopen: nothing rendered for the new document unasked");
    e = workerLookup(0, workerScaleKey(1.0f), 8);
    check(e == NULL, "reopen: the old document's pages are gone from the cache");
    workerUnpin(e);
    (void)oldGen;
    flushKept();
    workerRender(0, 1.0f, 8, WPRIO_VISIBLE);
    r = await(WR_RENDERED, 0, gen, 10000);
    check(r != NULL && r->docGen == gen, "reopen: the new document renders");
    workerFreeResult(r);
  }

  /* 9b. Aborting a page half drawn, proven by the worker's own count: the
     other document's heaviest page (argument 4, 1-based; TFMS5360.pdf's
     page 5 has 5,600 separate strokes) drawn in full once, then again at a
     slightly different scale and dropped a third of the way through. */
  if(argc > 4) {
    LONG hp = atol(argv[4]) - 1;
    ULONG full, before;
    workerRender(hp, 4.0f, 8, WPRIO_VISIBLE);
    r = await(WR_RENDERED, hp, gen, 120000);
    full = r ? r->drawMs : 0;
    printf("      heavy page at 4x draws in %lu ms\n", (unsigned long)full);
    workerFreeResult(r);
    before = workerAborts();
    workerRender(hp, 4.01f, 8, WPRIO_VISIBLE);
    usleep((useconds_t)(full * 1000 / 3));
    workerRetain(NULL, 0);
    r = await(WR_RENDERED, hp, gen, (int)full * 3 + 500);
    check(r == NULL, "abort in flight: the page is not delivered");
    workerFreeResult(r);
    check(workerAborts() == before + 1, "abort in flight: the worker abandoned the drawing half way");
  }

  // 10. Stop
  workerStop();
  check(1, "worker stops");
  printf("%d failure(s)\n", failures);
  return failures;
}

/**
 * WarpPDFView - a PDF viewer for Picasso96 true-colour screens.
 *
 * An MUI window: a toolbar (Open, page navigation, zoom, anti-aliasing, a
 * status field), the pages (PageView, pageview.c) with their scrollers, and
 * a find bar.  All MuPDF work happens in the render worker (worker.c); this
 * side only decides which pages it wants, at which scale, and shows what
 * comes back.
 *
 * Two view modes (menu View):
 *   - Single page: one page at a time; scrolling past its end with the wheel
 *     or the keys turns the page.
 *   - Continuous: all pages stacked.  A page not rendered yet is a white
 *     placeholder with its number; the pages in view render first, from the
 *     middle out, and a page scrolled past before it is done is abandoned
 *     (workerRetain aborts it).
 * Either way the next two pages and the previous one are rendered ahead in
 * idle time, and rendered pages stay in a cache (Settings/Caching) for going
 * back.
 *
 * Keys: Space/Backspace next/previous page (a screenful in continuous mode),
 * n/p next/previous page, cursor keys scroll (Shift: a screenful), keypad
 * 9/3 page up/down and 7/1 first/last, + and - zoom, w fit width, f fit
 * page, g type a page number, o open, q/Esc quit; Amiga-F find, Amiga-G/H
 * next/previous hit.
 *
 *   WarpPDFView [FILE] [PAGE n] [DPI n] [AA bits]
 *
 * Started from Workbench it takes a project icon's file, or asks for one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <libraries/iffparse.h>   // MAKE_ID
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>

#include "worker.h"
#include "pageview.h"

// MuPDF recurses deeply - in the worker, which gets its own stack; this is for the UI.
size_t __stack = 64 * 1024;
static const char stackCookie[] = "$STACK: 65536";
#define VERSION_STRING "WarpPDFView 0.4 (5.10.2026)"
static const char versionTag[] = "$VER: " VERSION_STRING;
/* AGPL-3.0 section 13: users must be offered the corresponding source. */
#define SOURCE_URL "https://github.com/andrzej79/WarpPDFView"

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *MUIMasterBase = NULL;
struct Library *AslBase = NULL;
struct Library *P96Base = NULL;
struct Library *UtilityBase = NULL;

// Return IDs: toolbar, keys and menus.
enum {
  ID_OPEN = 1, ID_ABOUT, ID_FIRST, ID_PREV, ID_NEXT, ID_LAST, ID_PAGE, ID_ZOOM, ID_AA,
  ID_KEY_NEXT, ID_KEY_PREV, ID_ZOOM_IN, ID_ZOOM_OUT, ID_FIT_WIDTH, ID_FIT_PAGE, ID_GOTO,
  ID_UP, ID_DOWN, ID_LEFT, ID_RIGHT, ID_PGUP, ID_PGDN, ID_SLEFT, ID_SRIGHT,
  ID_MODE_SINGLE, ID_MODE_CONT,
  ID_FIND, ID_FIND_NEXT, ID_FIND_PREV,
  ID_CACHE_4, ID_CACHE_8, ID_CACHE_16, ID_CACHE_32, ID_CACHE_64,
  ID_BOOKMARKS, ID_BOOKMARK, ID_K_BOOKMARKS,
  // Letter keys: ignored while a text box is being typed into
  ID_K_NEXT, ID_K_PREV, ID_K_OPEN, ID_K_FITW, ID_K_FITP, ID_K_FIRST, ID_K_LAST, ID_K_PGUP, ID_K_PGDN,
  ID_K_QUIT, ID_K_ESC
};

// Zoom: the first two fit the view, the rest are fixed resolutions.
#define ZOOM_FITWIDTH 0
#define ZOOM_FITPAGE  1
static STRPTR zoomLabels[] = {
  (STRPTR) "Fit width", (STRPTR) "Fit page", (STRPTR) "36 dpi", (STRPTR) "50 dpi", (STRPTR) "72 dpi",
  (STRPTR) "96 dpi", (STRPTR) "120 dpi", (STRPTR) "144 dpi", (STRPTR) "200 dpi", NULL
};
static const int zoomDpi[] = { 0, 0, 36, 50, 72, 96, 120, 144, 200 };
#define ZOOM_COUNT 9

static STRPTR aaLabels[] = { (STRPTR) "AA 8 bit", (STRPTR) "AA 6 bit", (STRPTR) "AA 4 bit", (STRPTR) "AA off", NULL };
static const int aaBits[] = { 8, 6, 4, 0 };

#define MODE_SINGLE 0
#define MODE_CONT   1
#define PAGE_GAP    8       // between pages in continuous mode
#define MAX_WANTED  64

static const ULONG cacheMB[] = { 4, 8, 16, 32, 64 };
#define CACHE_SIZES 5

static struct NewMenu menuDef[] = {
  { NM_TITLE, (STRPTR) "Project", NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Open...", (STRPTR) "O", 0, 0, (APTR)ID_OPEN },
  { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "About...", (STRPTR) "?", 0, 0, (APTR)ID_ABOUT },
  { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Quit", (STRPTR) "Q", 0, 0, (APTR)MUIV_Application_ReturnID_Quit },
  { NM_TITLE, (STRPTR) "View", NULL, 0, 0, NULL },
  // Mutual exclusion counts items from the top of the menu, bars included.
  { NM_ITEM, (STRPTR) "Single page", (STRPTR) "1", CHECKIT, 0x2, (APTR)ID_MODE_SINGLE },
  { NM_ITEM, (STRPTR) "Continuous", (STRPTR) "2", CHECKIT, 0x1, (APTR)ID_MODE_CONT },
  { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Fit width", (STRPTR) "W", 0, 0, (APTR)ID_FIT_WIDTH },
  { NM_ITEM, (STRPTR) "Fit page", (STRPTR) "P", 0, 0, (APTR)ID_FIT_PAGE },
  { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Bookmarks", (STRPTR) "B", CHECKIT | MENUTOGGLE, 0, (APTR)ID_BOOKMARKS },
  { NM_TITLE, (STRPTR) "Search", NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Find...", (STRPTR) "F", 0, 0, (APTR)ID_FIND },
  { NM_ITEM, (STRPTR) "Find next", (STRPTR) "G", 0, 0, (APTR)ID_FIND_NEXT },
  { NM_ITEM, (STRPTR) "Find previous", (STRPTR) "H", 0, 0, (APTR)ID_FIND_PREV },
  { NM_TITLE, (STRPTR) "Settings", NULL, 0, 0, NULL },
  { NM_ITEM, (STRPTR) "Caching", NULL, 0, 0, NULL },
  { NM_SUB, (STRPTR) "4 MB", NULL, CHECKIT, 0x1E, (APTR)ID_CACHE_4 },
  { NM_SUB, (STRPTR) "8 MB", NULL, CHECKIT, 0x1D, (APTR)ID_CACHE_8 },
  { NM_SUB, (STRPTR) "16 MB", NULL, CHECKIT, 0x1B, (APTR)ID_CACHE_16 },
  { NM_SUB, (STRPTR) "32 MB", NULL, CHECKIT, 0x17, (APTR)ID_CACHE_32 },
  { NM_SUB, (STRPTR) "64 MB", NULL, CHECKIT, 0x0F, (APTR)ID_CACHE_64 },
  { NM_END, NULL, NULL, 0, 0, NULL }
};
#define MENU_SINGLE    7
#define MENU_CONT      8
#define MENU_BOOKMARKS 13
#define MENU_CACHE0    20

static struct {
  Object *app, *win;
  Object *bOpen, *bFirst, *bPrev, *bNext, *bLast;
  Object *sPage, *tTotal, *cZoom, *cAA, *tStatus;
  Object *sFind, *bFindNext, *bFindPrev, *tFind;
  Object *strip, *lvBookmarks, *lBookmarks, *balance;
  Object *view, *hbar, *vbar;
  ULONG workerSig;

  // The document
  BOOL open;
  ULONG docGen;
  char path[512];
  LONG count;
  float firstW, firstH;

  // How it is shown
  int mode;
  int zoom;
  int aa;
  float scale;
  ULONG scaleKey;
  LONG cur;                   // the current page, 0-based

  PvSlot *slots;              // count of them (single page mode uses the first)
  CacheEntry **pinned;        // per page: the cached page shown, pinned
  LONG nslots;
  LONG canvasW, canvasH;
  ULONG serial;
  ULONG lastListMs, lastDrawMs;
  BOOL inCallback;

  // Search
  char needle[128];
  ULONG searchGen;
  BOOL searching;
  LONG hitPage;               // -1: no hits
  LONG hitCount;
  LONG hitActive;
  float hitPts[WORKER_MAX_HITS * 4];
  PvRect hitRects[WORKER_MAX_HITS];

  // Bookmarks: the outline, flattened; shown in the side panel
  WorkerBookmark *bm;
  LONG bmCount;
  LONG bmActive;              // the entry highlighted for the current page, -1: none
  BOOL bmWanted;              // the user's choice (View/Bookmarks), kept in ENVARC:
  BOOL bmShown;               // the panel is on screen (tracked here, not read back from MUI)

  int cacheIdx;
  char status[96];
  char findStatus[64];
  char total[16];
  char title[160];
} v;

// ---- helpers ------------------------------------------------------------------------

static ULONG xget(Object *obj, ULONG attr)
{
  ULONG x = 0;

  get(obj, attr, &x);
  return x;
}

static void message(const char *text)
{
  if(v.app != NULL) {
    MUI_Request(v.app, v.win, 0, (STRPTR) "WarpPDFView", (STRPTR) "*_OK", (STRPTR) "%s", (ULONG)text);
  } else {
    printf("WarpPDFView: %s\n", text);
  }
}

static void setStatus(const char *text)
{
  // Continuous scrolling asks on every move: only a change is drawn.
  if(strcmp(text, v.status) == 0) {
    return;
  }
  strncpy(v.status, text, sizeof(v.status) - 1);
  v.status[sizeof(v.status) - 1] = 0;
  if(v.tStatus != NULL) {
    set(v.tStatus, MUIA_Text_Contents, (ULONG)v.status);
  }
}

static void setFindStatus(const char *text)
{
  strncpy(v.findStatus, text, sizeof(v.findStatus) - 1);
  v.findStatus[sizeof(v.findStatus) - 1] = 0;
  if(v.tFind != NULL) {
    set(v.tFind, MUIA_Text_Contents, (ULONG)v.findStatus);
  }
}

static LONG viewW(void)
{
  LONG w = (LONG)xget(v.view, MUIA_PageView_ViewWidth);
  return (w > 0) ? w : 600;
}

static LONG viewH(void)
{
  LONG h = (LONG)xget(v.view, MUIA_PageView_ViewHeight);
  return (h > 0) ? h : 800;
}

static void pageSize(LONG page, float *w, float *h)
{
  if(!workerPageSize(page, w, h)) {
    *w = v.firstW;
    *h = v.firstH;
  }
  if(*w < 1.0f) {
    *w = 1.0f;
  }
  if(*h < 1.0f) {
    *h = 1.0f;
  }
}

// ---- settings ------------------------------------------------------------------------

static int getVarInt(const char *name, int def)
{
  char buf[16];

  if(GetVar((STRPTR)name, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
    return atoi(buf);
  }
  return def;
}

static void setVarInt(const char *name, int value)
{
  char buf[16];

  snprintf(buf, sizeof(buf), "%d", value);
  SetVar((STRPTR)name, (STRPTR)buf, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

// ---- pages on screen ----------------------------------------------------------------

static float scaleFor(int zoom)
{
  float pw, ph, s;

  pageSize(v.cur, &pw, &ph);
  if(zoom == ZOOM_FITWIDTH) {
    s = (float)viewW() / pw;
  } else if(zoom == ZOOM_FITPAGE) {
    s = (float)viewW() / pw;
    if((float)viewH() / ph < s) {
      s = (float)viewH() / ph;
    }
  } else {
    s = (float)zoomDpi[zoom] / 72.0f;
  }
  // Nothing smaller than a thumbnail, nothing past ~600 dpi.
  if(s < 0.1f) {
    s = 0.1f;
  }
  if(s > 8.0f) {
    s = 8.0f;
  }
  return s;
}

static LONG scaled(float points)
{
  // As MuPDF rounds a page out to whole pixels.
  return (LONG)(points * v.scale + 0.999f);
}

static LONG slotOf(LONG page)
{
  if(v.slots == NULL || page < 0 || page >= v.count) {
    return -1;
  }
  if(v.mode == MODE_CONT) {
    return page;
  }
  return (page == v.cur) ? 0 : -1;
}

// The search hits in the slot showing their page, at the current scale.
static void placeHits(void)
{
  LONG i, s;

  for(i = 0; i < v.nslots; i++) {
    v.slots[i].hits = NULL;
    v.slots[i].hitCount = 0;
    v.slots[i].hitActive = -1;
  }
  if(v.hitPage < 0) {
    return;
  }
  for(i = 0; i < v.hitCount; i++) {
    v.hitRects[i].x0 = (LONG)(v.hitPts[i * 4 + 0] * v.scale);
    v.hitRects[i].y0 = (LONG)(v.hitPts[i * 4 + 1] * v.scale);
    v.hitRects[i].x1 = (LONG)(v.hitPts[i * 4 + 2] * v.scale + 0.999f);
    v.hitRects[i].y1 = (LONG)(v.hitPts[i * 4 + 3] * v.scale + 0.999f);
  }
  s = slotOf(v.hitPage);
  if(s >= 0) {
    v.slots[s].hits = v.hitRects;
    v.slots[s].hitCount = v.hitCount;
    v.slots[s].hitActive = v.hitActive;
  }
}

static void unpinAll(void)
{
  LONG i;

  if(v.pinned == NULL) {
    return;
  }
  for(i = 0; i < v.count; i++) {
    if(v.pinned[i] != NULL) {
      workerUnpin(v.pinned[i]);
      v.pinned[i] = NULL;
    }
  }
}

/* Lay the pages out at the current scale.  keep: only the positions may have
   changed (page sizes arriving), the pages in the slots are the same. */
static void layout(BOOL keep)
{
  LONG i, y = 0, maxW = 0;

  if(!v.open || v.slots == NULL) {
    DoMethod(v.view, MUIM_PageView_SetLayout, NULL, 0, 0, 0, FALSE);
    return;
  }
  if(v.mode == MODE_SINGLE) {
    float pw, ph;
    PvSlot *s = &v.slots[0];
    pageSize(v.cur, &pw, &ph);
    s->x = 0;
    s->y = 0;
    s->w = scaled(pw);
    s->h = scaled(ph);
    s->page = v.cur;
    s->img = (v.pinned[v.cur] != NULL) ? workerImage(v.pinned[v.cur]) : NULL;
    s->serial = ++v.serial;
    v.nslots = 1;
    v.canvasW = s->w;
    v.canvasH = s->h;
  } else {
    for(i = 0; i < v.count; i++) {
      float pw, ph;
      PvSlot *s = &v.slots[i];
      pageSize(i, &pw, &ph);
      s->w = scaled(pw);
      s->h = scaled(ph);
      s->y = y;
      s->page = i;
      if(!keep) {
        s->img = (v.pinned[i] != NULL) ? workerImage(v.pinned[i]) : NULL;
        s->serial = ++v.serial;
      }
      y += s->h + PAGE_GAP;
      if(s->w > maxW) {
        maxW = s->w;
      }
    }
    for(i = 0; i < v.count; i++) {
      v.slots[i].x = (maxW - v.slots[i].w) / 2;
    }
    v.nslots = v.count;
    v.canvasW = maxW;
    v.canvasH = (y > PAGE_GAP) ? y - PAGE_GAP : 0;
  }
  placeHits();
  DoMethod(v.view, MUIM_PageView_SetLayout, (ULONG)v.slots, v.nslots, v.canvasW, v.canvasH, (ULONG)keep);
}

// Show the pinned page in its slot, if it has one on screen.
static void showPinned(LONG page)
{
  LONG s = slotOf(page);

  if(s < 0) {
    return;
  }
  v.slots[s].img = (v.pinned[page] != NULL) ? workerImage(v.pinned[page]) : NULL;
  v.slots[s].serial = ++v.serial;
  DoMethod(v.view, MUIM_PageView_SlotChanged, s);
}

// The page from the cache, if it is there: pinned and shown.
static BOOL attach(LONG page)
{
  CacheEntry *e;

  if(v.pinned[page] != NULL) {
    return TRUE;
  }
  e = workerLookup(page, v.scaleKey, aaBits[v.aa]);
  if(e == NULL) {
    return FALSE;
  }
  v.pinned[page] = e;
  showPinned(page);
  return TRUE;
}

static void detach(LONG page)
{
  if(v.pinned[page] != NULL) {
    workerUnpin(v.pinned[page]);
    v.pinned[page] = NULL;
    showPinned(page);
  }
}

/* The panel shows when the user wants it and the document has bookmarks. */
static void showBookmarks(void)
{
  BOOL on = (v.bmWanted && v.bmCount > 0) ? TRUE : FALSE;

  if(v.lvBookmarks != NULL && on != v.bmShown) {
    v.bmShown = on;
    set(v.lvBookmarks, MUIA_ShowMe, (ULONG)on);
    set(v.balance, MUIA_ShowMe, (ULONG)on);
  }
}

// What the document's bookmarks are, where the find bar reports.
static void bookmarkStatus(void)
{
  char st[64];

  if(v.bmCount == 0) {
    strcpy(st, "No bookmarks in this document");
  } else if(!v.bmWanted) {
    snprintf(st, sizeof(st), "%ld bookmarks, b shows", (long)v.bmCount);
  } else {
    snprintf(st, sizeof(st), "%ld bookmarks", (long)v.bmCount);
  }
  setFindStatus(st);
}

/* Highlight the bookmark for the current page: the last one before or on it.
   Quietly - a highlight set from here must not jump anywhere. */
static void syncBookmark(void)
{
  LONG i, best = -1;

  if(v.bm == NULL) {
    return;
  }
  // A bookmark chosen on this page stays chosen, even if others share the page.
  if(v.bmActive >= 0 && v.bmActive < v.bmCount && v.bm[v.bmActive].page == v.cur) {
    return;
  }
  for(i = 0; i < v.bmCount; i++) {
    if(v.bm[i].page >= 0 && v.bm[i].page <= v.cur) {
      best = i;
    }
  }
  if(best != v.bmActive) {
    v.bmActive = best;
    nnset(v.lBookmarks, MUIA_List_Active, (best >= 0) ? (ULONG)best : (ULONG)MUIV_List_Active_Off);
  }
}

static void updateToolbar(void)
{
  snprintf(v.total, sizeof(v.total), "/ %ld", (long)(v.open ? v.count : 0));
  set(v.tTotal, MUIA_Text_Contents, (ULONG)v.total);
  nnset(v.sPage, MUIA_String_Integer, (ULONG)(v.cur + 1));
  nnset(v.cZoom, MUIA_Cycle_Active, (ULONG)v.zoom);
  syncBookmark();
}

static void showStatus(void)
{
  char st[96];

  if(!v.open) {
    return;
  }
  if(v.pinned[v.cur] == NULL) {
    snprintf(st, sizeof(st), "%ld dpi  rendering page %ld...", (long)(v.scale * 72.0f + 0.5f), (long)(v.cur + 1));
  } else if(v.lastDrawMs == 0 && v.lastListMs == 0) {
    snprintf(st, sizeof(st), "%ld dpi  from the cache", (long)(v.scale * 72.0f + 0.5f));
  } else {
    snprintf(st, sizeof(st), "%ld dpi  list %lu ms  draw %lu ms", (long)(v.scale * 72.0f + 0.5f), v.lastListMs,
             v.lastDrawMs);
  }
  setStatus(st);
}

/* Which pages are wanted now, and the worker told so: the ones in view
   first, from the middle out, then the next two and the previous one ahead.
   Everything else queued is dropped, and a page no longer wanted stops
   drawing.  In continuous mode the current page follows the middle of the
   view. */
static void updateWanted(void)
{
  RenderKey keys[MAX_WANTED];
  LONG wanted[MAX_WANTED];
  int nk = 0, nw = 0, i;
  LONG first, last, p;
  int aa = aaBits[v.aa];

  if(!v.open || v.slots == NULL) {
    return;
  }
  if(v.mode == MODE_SINGLE) {
    first = last = v.cur;
  } else {
    /* The current page is the one a quarter down the view: a page gone to
       starts at the top, and a short one (fit page) must not hand over to
       the next as soon as that shows. */
    LONG top = (LONG)xget(v.view, MUIA_PageView_Top), vh = viewH(), mid = top + vh / 4, newCur = v.cur;
    if(v.canvasH <= vh) {
      top = 0;
      mid = v.canvasH / 4;
    }
    first = last = -1;
    for(p = 0; p < v.count; p++) {
      PvSlot *s = &v.slots[p];
      if(s->y + s->h > top && s->y < top + vh) {
        if(first < 0) {
          first = p;
        }
        last = p;
      }
      if(s->y <= mid && s->y + s->h + PAGE_GAP > mid) {
        newCur = p;
      }
    }
    if(first < 0) {
      first = last = v.cur;
    }
    if(newCur != v.cur) {
      v.cur = newCur;
      nnset(v.sPage, MUIA_String_Integer, (ULONG)(v.cur + 1));
      syncBookmark();
    }
  }

  // In view: the current page first, then outwards.
  wanted[nw++] = v.cur;
  for(i = 1; nw < MAX_WANTED - 3 && (v.cur - i >= first || v.cur + i <= last); i++) {
    if(v.cur + i <= last) {
      wanted[nw++] = v.cur + i;
    }
    if(v.cur - i >= first) {
      wanted[nw++] = v.cur - i;
    }
  }
  for(i = 0; i < nw; i++) {
    p = wanted[i];
    if(!attach(p)) {
      workerRender(p, v.scale, aa, WPRIO_VISIBLE);
    }
    keys[nk].page = p;
    keys[nk].scaleKey = v.scaleKey;
    keys[nk].aa = aa;
    nk++;
  }
  // Ahead: the next two, the previous one.
  for(p = last + 1; p <= last + 2 && p < v.count; p++) {
    workerRender(p, v.scale, aa, WPRIO_PREFETCH);
    keys[nk].page = p;
    keys[nk].scaleKey = v.scaleKey;
    keys[nk].aa = aa;
    nk++;
  }
  if(first > 0) {
    workerRender(first - 1, v.scale, aa, WPRIO_PREFETCH);
    keys[nk].page = first - 1;
    keys[nk].scaleKey = v.scaleKey;
    keys[nk].aa = aa;
    nk++;
  }
  workerRetain(keys, nk);

  // Pages far out of view give their pins back: they stay cached, not shown.
  for(p = 0; p < v.count; p++) {
    if(v.pinned[p] != NULL && (p < first - 2 || p > last + 2)) {
      detach(p);
    }
  }
  showStatus();
}

// A new scale (zoom, window size, anti-aliasing): everything shown is out of date.
static void rescale(void)
{
  float s = scaleFor(v.zoom);
  ULONG key = workerScaleKey(s);
  LONG anchorOff = 0, top = 0, left;
  float frac = 0.0f;

  if(!v.open) {
    return;
  }
  if(v.mode == MODE_CONT && v.slots != NULL && v.slots[v.cur].h > 0) {
    // Keep the current page where it is, proportionally.
    top = (LONG)xget(v.view, MUIA_PageView_Top);
    anchorOff = top - v.slots[v.cur].y;
    frac = (float)anchorOff / (float)v.slots[v.cur].h;
  }
  left = (LONG)xget(v.view, MUIA_PageView_Left);
  if(key != v.scaleKey) {
    unpinAll();
    left = (v.scale > 0.0f) ? (LONG)((float)left * s / v.scale) : 0;
  }
  v.scale = s;
  v.scaleKey = key;
  v.inCallback = TRUE;
  layout(FALSE);
  if(v.mode == MODE_CONT) {
    top = v.slots[v.cur].y + (LONG)(frac * (float)v.slots[v.cur].h);
    SetAttrs(v.view, MUIA_PageView_Top, (ULONG)top, MUIA_PageView_Left, (ULONG)left, TAG_DONE);
  } else {
    SetAttrs(v.view, MUIA_PageView_Left, (ULONG)left, TAG_DONE);
  }
  v.inCallback = FALSE;
  updateToolbar();
  updateWanted();
}

/* Go to page (0-based).  toBottom: arriving from the next page by scrolling
   up, so show the page's end. */
static void gotoPage(LONG page, BOOL toBottom)
{
  if(!v.open) {
    return;
  }
  if(page < 0) {
    page = 0;
  }
  if(page >= v.count) {
    page = v.count - 1;
  }
  if(v.mode == MODE_CONT) {
    v.cur = page;
    v.inCallback = TRUE;
    SetAttrs(v.view, MUIA_PageView_Top, (ULONG)v.slots[page].y, TAG_DONE);
    v.inCallback = FALSE;
    updateToolbar();
    updateWanted();
    return;
  }
  if(page == v.cur && v.slots[0].page == page) {
    updateToolbar();
    return;
  }
  /* The page left stays in the cache, unpinned: going back finds it there.
     Quietly - the slot is laid out again below, nothing is drawn between. */
  if(v.pinned[v.cur] != NULL) {
    workerUnpin(v.pinned[v.cur]);
    v.pinned[v.cur] = NULL;
  }
  v.cur = page;
  v.lastListMs = v.lastDrawMs = 0;
  {
    float s = scaleFor(v.zoom);
    if(workerScaleKey(s) != v.scaleKey) {
      unpinAll();
      v.scale = s;
      v.scaleKey = workerScaleKey(s);
    }
  }
  v.inCallback = TRUE;
  layout(FALSE);
  SetAttrs(v.view, MUIA_PageView_Top, (ULONG)(toBottom ? 0x7FFFFFFF : 0), MUIA_PageView_Left, 0, TAG_DONE);
  v.inCallback = FALSE;
  updateToolbar();
  updateWanted();
}

static void setMode(int mode)
{
  if(mode == v.mode) {
    return;
  }
  v.mode = mode;
  setVarInt("WarpPDFView.mode", mode);
  set(v.view, MUIA_PageView_FlipAtEnds, (mode == MODE_SINGLE) ? TRUE : FALSE);
  if(!v.open) {
    return;
  }
  v.inCallback = TRUE;
  layout(FALSE);
  if(mode == MODE_CONT) {
    SetAttrs(v.view, MUIA_PageView_Top, (ULONG)v.slots[v.cur].y, TAG_DONE);
  } else {
    // Only the current page stays pinned.
    LONG p;
    for(p = 0; p < v.count; p++) {
      if(p != v.cur) {
        detach(p);
      }
    }
    layout(FALSE);
    SetAttrs(v.view, MUIA_PageView_Top, 0, TAG_DONE);
  }
  v.inCallback = FALSE;
  updateWanted();
}

static void setZoom(int zoom)
{
  if(zoom < 0 || zoom >= ZOOM_COUNT) {
    return;
  }
  v.zoom = zoom;
  updateToolbar();
  rescale();
}

// +/-: the next fixed resolution up or down from the current scale.
static void stepZoom(int dir)
{
  int i;
  float dpi = v.scale * 72.0f;

  if(dir > 0) {
    for(i = ZOOM_FITPAGE + 1; i < ZOOM_COUNT; i++) {
      if((float)zoomDpi[i] > dpi + 1.0f) {
        setZoom(i);
        return;
      }
    }
  } else {
    for(i = ZOOM_COUNT - 1; i > ZOOM_FITPAGE; i--) {
      if((float)zoomDpi[i] < dpi - 1.0f) {
        setZoom(i);
        return;
      }
    }
  }
}

/* Go to a bookmark: its page, and where on it the bookmark points, a little
   above so the heading shows. */
static void gotoBookmark(LONG idx)
{
  LONG y, top;

  if(!v.open || v.bm == NULL || idx < 0 || idx >= v.bmCount || v.bm[idx].page < 0 ||
     v.bm[idx].page >= v.count) {
    return;
  }
  y = (LONG)(v.bm[idx].y * v.scale) - 12;
  if(y < 0) {
    y = 0;
  }
  // The highlight is where the user put it, not the page's own last bookmark.
  v.bmActive = idx;
  gotoPage(v.bm[idx].page, FALSE);
  top = ((v.mode == MODE_CONT) ? v.slots[v.bm[idx].page].y : 0) + y;
  SetAttrs(v.view, MUIA_PageView_Top, (ULONG)top, TAG_DONE);
}

// ---- the view's callback -----------------------------------------------------------

static void viewEvent(LONG event, LONG value)
{
  // Moves this code makes itself are followed up by it.
  if(v.inCallback) {
    return;
  }
  v.inCallback = TRUE;
  switch(event) {
    case PVE_MOVED:
      if(v.mode == MODE_CONT) {
        updateWanted();
      }
      break;
    case PVE_FLIP:
      if(v.mode == MODE_SINGLE) {
        if(value > 0 && v.cur + 1 < v.count) {
          v.inCallback = FALSE;
          gotoPage(v.cur + 1, FALSE);
        } else if(value < 0 && v.cur > 0) {
          v.inCallback = FALSE;
          gotoPage(v.cur - 1, TRUE);
        }
      }
      break;
    case PVE_RESIZED:
      v.inCallback = FALSE;
      if(v.zoom == ZOOM_FITWIDTH || v.zoom == ZOOM_FITPAGE) {
        rescale();
      } else {
        updateWanted();
      }
      break;
  }
  v.inCallback = FALSE;
}

// ---- documents -----------------------------------------------------------------------

static void closeDocument(void)
{
  unpinAll();
  DoMethod(v.view, MUIM_PageView_SetLayout, NULL, 0, 0, 0, FALSE);
  if(v.slots != NULL) {
    FreeVec(v.slots);
    v.slots = NULL;
  }
  if(v.pinned != NULL) {
    FreeVec(v.pinned);
    v.pinned = NULL;
  }
  v.open = FALSE;
  v.count = 0;
  v.nslots = 0;
  v.hitPage = -1;
  v.hitCount = 0;
  if(v.lBookmarks != NULL) {
    DoMethod(v.lBookmarks, MUIM_List_Clear);
  }
  if(v.bm != NULL) {
    FreeVec(v.bm);
    v.bm = NULL;
  }
  v.bmCount = 0;
  v.bmActive = -1;
  showBookmarks();
}

static void openDocument(const char *path)
{
  closeDocument();
  strncpy(v.path, path, sizeof(v.path) - 1);
  v.path[sizeof(v.path) - 1] = 0;
  v.docGen = workerOpen(path);
  v.searching = FALSE;
  setStatus("Opening...");
  setFindStatus("");
}

static void setTitle(void)
{
  if(v.open) {
    snprintf(v.title, sizeof(v.title), "WarpPDFView - %s", (const char *)FilePart((STRPTR)v.path));
  } else {
    strcpy(v.title, "WarpPDFView");
  }
  set(v.win, MUIA_Window_Title, (ULONG)v.title);
}

static void opened(WorkerResult *r, LONG startPage)
{
  v.count = r->pageCount;
  v.firstW = r->w;
  v.firstH = r->h;
  v.slots = (PvSlot *)AllocVec((ULONG)v.count * sizeof(PvSlot), MEMF_ANY | MEMF_CLEAR);
  v.pinned = (CacheEntry **)AllocVec((ULONG)v.count * sizeof(CacheEntry *), MEMF_ANY | MEMF_CLEAR);
  if(v.slots == NULL || v.pinned == NULL) {
    closeDocument();
    message("Not enough memory for the document's page table.");
    return;
  }
  v.open = TRUE;
  v.cur = (startPage >= 0 && startPage < v.count) ? startPage : 0;
  v.scale = scaleFor(v.zoom);
  v.scaleKey = workerScaleKey(v.scale);
  v.hitPage = -1;
  setTitle();
  v.inCallback = TRUE;
  layout(FALSE);
  SetAttrs(v.view, MUIA_PageView_Top, (ULONG)((v.mode == MODE_CONT) ? v.slots[v.cur].y : 0), MUIA_PageView_Left, 0,
           TAG_DONE);
  v.inCallback = FALSE;
  updateToolbar();
  updateWanted();
}

static BOOL askFile(char *out, ULONG size)
{
  struct FileRequester *fr;
  char drawer[512];
  BOOL ok = FALSE;

  if(AslBase == NULL) {
    return FALSE;
  }
  drawer[0] = 0;
  if(v.path[0] != 0) {
    STRPTR part = PathPart((STRPTR)v.path);
    ULONG n = (ULONG)(part - (STRPTR)v.path);
    if(n >= sizeof(drawer)) {
      n = sizeof(drawer) - 1;
    }
    memcpy(drawer, v.path, n);
    drawer[n] = 0;
  }
  fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (ULONG) "Open a PDF",
                                                    ASLFR_InitialPattern, (ULONG) "#?.pdf", ASLFR_DoPatterns, TRUE,
                                                    ASLFR_InitialDrawer, (ULONG)drawer, TAG_DONE);
  if(fr == NULL) {
    return FALSE;
  }
  set(v.app, MUIA_Application_Sleep, TRUE);
  if(AslRequestTags(fr, ASLFR_Window, xget(v.win, MUIA_Window_Window), TAG_DONE)) {
    strncpy(out, (const char *)fr->fr_Drawer, size - 1);
    out[size - 1] = 0;
    ok = AddPart((STRPTR)out, fr->fr_File, size) ? TRUE : FALSE;
  }
  set(v.app, MUIA_Application_Sleep, FALSE);
  FreeAslRequest(fr);
  return ok;
}

// ---- search --------------------------------------------------------------------------

// Bring the current hit into view.
static void showHit(void)
{
  LONG s, top, left, vh = viewH(), vw = viewW();
  const PvRect *r;

  placeHits();
  s = slotOf(v.hitPage);
  if(s < 0 || v.hitActive < 0 || v.hitActive >= v.hitCount) {
    return;
  }
  DoMethod(v.view, MUIM_PageView_SlotChanged, s);
  r = &v.hitRects[v.hitActive];
  top = (LONG)xget(v.view, MUIA_PageView_Top);
  left = (LONG)xget(v.view, MUIA_PageView_Left);
  if(v.slots[s].y + r->y0 < top || v.slots[s].y + r->y1 > top + vh) {
    top = v.slots[s].y + (r->y0 + r->y1) / 2 - vh / 2;
  }
  if(v.slots[s].x + r->x0 < left || v.slots[s].x + r->x1 > left + vw) {
    left = v.slots[s].x + (r->x0 + r->x1) / 2 - vw / 2;
  }
  SetAttrs(v.view, MUIA_PageView_Top, (ULONG)((top > 0) ? top : 0), MUIA_PageView_Left,
           (ULONG)((left > 0) ? left : 0), TAG_DONE);
}

static void startSearch(LONG page, int dir)
{
  if(!v.open || v.needle[0] == 0) {
    return;
  }
  v.searchGen = workerSearch(v.needle, page, dir);
  v.searching = TRUE;
  setFindStatus("Searching...");
}

/* Next or previous hit: on the same page while there are more, else the
   worker looks further, from the page after (or before) the one with hits. */
static void findStep(int dir)
{
  char text[128];
  STRPTR s = (STRPTR)xget(v.sFind, MUIA_String_Contents);

  strncpy(text, (s != NULL) ? (const char *)s : "", sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  if(text[0] == 0) {
    // An empty box clears the marks.
    if(v.hitPage >= 0) {
      LONG slot = slotOf(v.hitPage);
      v.hitPage = -1;
      v.hitCount = 0;
      placeHits();
      if(slot >= 0) {
        DoMethod(v.view, MUIM_PageView_SlotChanged, slot);
      }
    }
    workerCancelSearch();
    v.searching = FALSE;
    setFindStatus("");
    return;
  }
  if(strcmp(text, v.needle) != 0 || v.hitPage < 0) {
    // A new search, from the current page.
    strcpy(v.needle, text);
    startSearch(v.cur, dir);
    return;
  }
  if(dir > 0 && v.hitActive + 1 < v.hitCount) {
    v.hitActive++;
    showHit();
  } else if(dir < 0 && v.hitActive > 0) {
    v.hitActive--;
    showHit();
  } else {
    LONG next = v.hitPage + dir;
    if(next >= v.count) {
      next = 0;
    } else if(next < 0) {
      next = v.count - 1;
    }
    startSearch(next, dir);
  }
}

static void searchHit(WorkerResult *r)
{
  char st[64];
  LONG old = slotOf(v.hitPage);

  v.searching = FALSE;
  v.hitPage = r->page;
  v.hitCount = r->hitCount;
  memcpy(v.hitPts, r->hits, sizeof(float) * 4 * (ULONG)r->hitCount);
  v.hitActive = (r->searchDir > 0) ? 0 : r->hitCount - 1;
  if(old >= 0) {
    placeHits();
    DoMethod(v.view, MUIM_PageView_SlotChanged, old);
  }
  snprintf(st, sizeof(st), "Page %ld: %ld hit%s", (long)(r->page + 1), (long)r->hitCount,
           (r->hitCount == 1) ? "" : "s");
  setFindStatus(st);
  if(v.mode == MODE_SINGLE && r->page != v.cur) {
    gotoPage(r->page, FALSE);
  } else if(v.mode == MODE_CONT) {
    v.cur = r->page;
  }
  showHit();
  updateToolbar();
  updateWanted();
}

// ---- results from the worker --------------------------------------------------------

static LONG startPage = 0;

static void handleResult(WorkerResult *r)
{
  char st[96];

  if(r->docGen != v.docGen) {
    return;   // for a document since replaced
  }
  switch(r->type) {
    case WR_OPENED:
      opened(r, startPage);
      startPage = 0;
      break;
    case WR_OPEN_FAILED:
      setStatus("");
      message(r->text);
      break;
    case WR_RENDERED:
      if(v.open && r->scaleKey == v.scaleKey && r->aa == aaBits[v.aa] && slotOf(r->page) >= 0) {
        /* Take it if it is wanted where it is: in view or next to it (the
           pins are given back in updateWanted as it goes out of range). */
        LONG s = slotOf(r->page);
        if(v.mode == MODE_SINGLE || (s >= 0 && v.pinned[r->page] == NULL)) {
          BOOL near = TRUE;
          if(v.mode == MODE_CONT) {
            LONG top = (LONG)xget(v.view, MUIA_PageView_Top), vh = viewH();
            near = (v.slots[s].y + v.slots[s].h > top - vh && v.slots[s].y < top + 2 * vh) ? TRUE : FALSE;
          }
          if(near && attach(r->page) && r->page == v.cur) {
            v.lastListMs = r->listMs;
            v.lastDrawMs = r->drawMs;
          }
        }
      }
      if(v.open && r->page == v.cur) {
        showStatus();
      }
      break;
    case WR_RENDER_FAILED:
      setStatus(r->text);
      break;
    case WR_SIZES:
      if(v.open && v.mode == MODE_CONT) {
        // Keep the current page where it is on screen.
        LONG top = (LONG)xget(v.view, MUIA_PageView_Top);
        LONG off = top - v.slots[v.cur].y;
        LONG oldH = v.canvasH, oldW = v.canvasW;
        v.inCallback = TRUE;
        layout(TRUE);
        if(v.canvasH != oldH || v.canvasW != oldW) {
          SetAttrs(v.view, MUIA_PageView_Top, (ULONG)(v.slots[v.cur].y + off), TAG_DONE);
        }
        v.inCallback = FALSE;
        updateWanted();
      }
      break;
    case WR_SEARCH_HIT:
      if(r->searchGen == v.searchGen) {
        searchHit(r);
      }
      break;
    case WR_SEARCH_PROGRESS:
      if(r->searchGen == v.searchGen && v.searching) {
        snprintf(st, sizeof(st), "Searching page %ld...", (long)(r->page + 1));
        setFindStatus(st);
      }
      break;
    case WR_OUTLINE:
      if(v.open) {
        LONG i;
        char line[128];
        // The list is filled quietly, then drawn once.
        v.bm = r->outline;
        v.bmCount = r->outlineCount;
        r->outline = NULL;
        v.bmActive = -1;
        set(v.lBookmarks, MUIA_List_Quiet, TRUE);
        DoMethod(v.lBookmarks, MUIM_List_Clear);
        for(i = 0; i < v.bmCount; i++) {
          int indent = v.bm[i].level * 3;
          if(indent > 30) {
            indent = 30;
          }
          snprintf(line, sizeof(line), "%*s%s", indent, "", v.bm[i].title);
          DoMethod(v.lBookmarks, MUIM_List_InsertSingle, (ULONG)line, MUIV_List_Insert_Bottom);
        }
        set(v.lBookmarks, MUIA_List_Quiet, FALSE);
        syncBookmark();
        showBookmarks();
        if(!v.searching) {
          bookmarkStatus();
        }
      }
      break;
    case WR_SEARCH_DONE:
      if(r->searchGen == v.searchGen) {
        v.searching = FALSE;
        setFindStatus((v.hitPage >= 0) ? "No more hits" : "Not found");
      }
      break;
  }
}

static void drainResults(void)
{
  WorkerResult *r;

  while((r = workerGetResult()) != NULL) {
    handleResult(r);
    workerFreeResult(r);
  }
}

// ---- the window ---------------------------------------------------------------------

static Object *button(const char *label, const char *help)
{
  return TextObject, ButtonFrame, MUIA_Background, MUII_ButtonBack, MUIA_Font, MUIV_Font_Button,
    MUIA_Text_Contents, (ULONG)label, MUIA_Text_PreParse, (ULONG) "\33c", MUIA_InputMode, MUIV_InputMode_RelVerify,
    MUIA_CycleChain, TRUE, MUIA_ShortHelp, (ULONG)help, End;
}

static BOOL buildApp(void)
{
  Object *strip;
  int i;

  // The menu's check marks from the settings.
  menuDef[MENU_SINGLE].nm_Flags = CHECKIT | ((v.mode == MODE_SINGLE) ? CHECKED : 0);
  menuDef[MENU_CONT].nm_Flags = CHECKIT | ((v.mode == MODE_CONT) ? CHECKED : 0);
  for(i = 0; i < CACHE_SIZES; i++) {
    menuDef[MENU_CACHE0 + i].nm_Flags = CHECKIT | ((i == v.cacheIdx) ? CHECKED : 0);
  }
  menuDef[MENU_BOOKMARKS].nm_Flags = CHECKIT | MENUTOGGLE | (v.bmWanted ? CHECKED : 0);
  strip = MUI_MakeObject(MUIO_MenustripNM, (ULONG)menuDef, 0);
  v.strip = strip;

  // The bookmarks panel: hidden until a document has an outline.
  v.lBookmarks = ListObject, InputListFrame, MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
    MUIA_List_DestructHook, MUIV_List_DestructHook_String, End;
  v.lvBookmarks = ListviewObject, MUIA_Listview_List, (ULONG)v.lBookmarks, MUIA_HorizWeight, 28, MUIA_ShowMe, FALSE,
    MUIA_CycleChain, TRUE, MUIA_ShortHelp, (ULONG) "Bookmarks: click to go there (View/Bookmarks, b)", End;
  v.balance = BalanceObject, MUIA_ShowMe, FALSE, End;

  v.bOpen = button("Open...", "Open a PDF (o)");
  v.bFirst = button("|<", "First page (keypad 7)");
  v.bPrev = button("<", "Previous page (Backspace, p)");
  v.bNext = button(">", "Next page (Space, n)");
  v.bLast = button(">|", "Last page (keypad 1)");
  v.sPage = StringObject, StringFrame, MUIA_String_Accept, (ULONG) "0123456789", MUIA_String_MaxLen, 7,
    MUIA_String_Integer, 1, MUIA_String_Format, MUIV_String_Format_Right, MUIA_FixWidthTxt, (ULONG) "000000",
    MUIA_CycleChain, TRUE, MUIA_ShortHelp, (ULONG) "Page number: type one and press Return (g)", End;
  v.tTotal = TextObject, MUIA_Text_Contents, (ULONG) "/ 0", MUIA_FixWidthTxt, (ULONG) "/ 00000", End;
  v.cZoom = CycleObject, MUIA_Cycle_Entries, (ULONG)zoomLabels, MUIA_Cycle_Active, v.zoom, MUIA_CycleChain, TRUE,
    MUIA_ShortHelp, (ULONG) "Zoom (+ and -, w fit width, f fit page)", End;
  v.cAA = CycleObject, MUIA_Cycle_Entries, (ULONG)aaLabels, MUIA_Cycle_Active, v.aa, MUIA_CycleChain, TRUE,
    MUIA_ShortHelp, (ULONG) "Anti-aliasing: fewer bits draw faster", End;
  v.tStatus = TextObject, TextFrame, MUIA_Background, MUII_TextBack, MUIA_Text_Contents, (ULONG) "", End;

  v.sFind = StringObject, StringFrame, MUIA_String_MaxLen, 100, MUIA_CycleChain, TRUE,
    MUIA_ShortHelp, (ULONG) "Text to find: Return finds the next (Amiga-F)", End;
  v.bFindPrev = button("<", "Previous hit (Amiga-H)");
  v.bFindNext = button(">", "Next hit (Amiga-G)");
  v.tFind = TextObject, MUIA_Text_Contents, (ULONG) "", MUIA_FixWidthTxt, (ULONG) "Searching page 00000...", End;

  v.hbar = ScrollbarObject, MUIA_Group_Horiz, TRUE, End;
  v.vbar = ScrollbarObject, End;
  v.view = PageViewObject, MUIA_FillArea, FALSE,
    MUIA_Background, (ULONG) "2:50505050,50505050,50505050",
    MUIA_PageView_HScroller, (ULONG)v.hbar, MUIA_PageView_VScroller, (ULONG)v.vbar,
    MUIA_PageView_Callback, (ULONG)viewEvent,
    MUIA_PageView_FlipAtEnds, (v.mode == MODE_SINGLE) ? TRUE : FALSE, End;

  v.win = WindowObject,
    MUIA_Window_Title, (ULONG) "WarpPDFView",
    MUIA_Window_ScreenTitle, (ULONG)VERSION_STRING,
    MUIA_Window_ID, MAKE_ID('W', 'P', 'D', 'F'),
    MUIA_Window_Width, MUIV_Window_Width_Screen(75),
    MUIA_Window_Height, MUIV_Window_Height_Screen(90),
    MUIA_Window_Menustrip, (ULONG)strip,
    MUIA_Window_RootObject, VGroup,
      Child, HGroup,
        Child, v.bOpen,
        Child, HSpace(8),
        Child, v.bFirst,
        Child, v.bPrev,
        Child, v.sPage,
        Child, v.tTotal,
        Child, v.bNext,
        Child, v.bLast,
        Child, HSpace(8),
        Child, v.cZoom,
        Child, v.cAA,
        Child, v.tStatus,
      End,
      Child, HGroup,
        Child, v.lvBookmarks,
        Child, v.balance,
        Child, ColGroup(2), MUIA_Group_Spacing, 0, MUIA_HorizWeight, 100,
          Child, v.view,
          Child, v.vbar,
          Child, v.hbar,
          Child, RectangleObject, End,
        End,
      End,
      Child, HGroup,
        Child, Label2((ULONG) "Find"),
        Child, v.sFind,
        Child, v.bFindPrev,
        Child, v.bFindNext,
        Child, v.tFind,
      End,
    End,
  End;

  if(v.win == NULL || v.view == NULL) {
    return FALSE;
  }
  v.app = ApplicationObject,
    MUIA_Application_Title, (ULONG) "WarpPDFView",
    MUIA_Application_Version, (ULONG)versionTag,
    MUIA_Application_Copyright, (ULONG) "(c) CS-Lab s.c.; MuPDF (c) Artifex Software; AGPL-3.0",
    MUIA_Application_Description, (ULONG) "PDF viewer for Picasso96 screens",
    MUIA_Application_Base, (ULONG) "WARPPDFVIEW",
    SubWindow, v.win,
  End;
  return (v.app != NULL) ? TRUE : FALSE;
}

static void key(const char *event, ULONG id)
{
  DoMethod(v.win, MUIM_Notify, MUIA_Window_InputEvent, (ULONG)event, v.app, 2, MUIM_Application_ReturnID, id);
}

static void notifications(void)
{
  Object *app = v.app;

  DoMethod(v.win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2, MUIM_Application_ReturnID,
           MUIV_Application_ReturnID_Quit);
  DoMethod(v.bOpen, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_OPEN);
  DoMethod(v.bFirst, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_FIRST);
  DoMethod(v.bPrev, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_PREV);
  DoMethod(v.bNext, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_NEXT);
  DoMethod(v.bLast, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_LAST);
  DoMethod(v.sPage, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_PAGE);
  DoMethod(v.cZoom, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_ZOOM);
  DoMethod(v.cAA, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_AA);
  DoMethod(v.sFind, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID,
           ID_FIND_NEXT);
  DoMethod(v.bFindNext, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_FIND_NEXT);
  DoMethod(v.bFindPrev, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_FIND_PREV);
  /* A bookmark is followed when it becomes the active one (click, or the
     cursor keys in the list), and again on a double click on the active one. */
  DoMethod(v.lBookmarks, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID,
           ID_BOOKMARK);
  DoMethod(v.lvBookmarks, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, app, 2, MUIM_Application_ReturnID,
           ID_BOOKMARK);

  // The scrollers move the view; the view keeps them in step itself.
  DoMethod(v.hbar, MUIM_Notify, MUIA_Prop_First, MUIV_EveryTime, v.view, 3, MUIM_Set, MUIA_PageView_Left,
           MUIV_TriggerValue);
  DoMethod(v.vbar, MUIM_Notify, MUIA_Prop_First, MUIV_EveryTime, v.view, 3, MUIM_Set, MUIA_PageView_Top,
           MUIV_TriggerValue);

  key("space", ID_KEY_NEXT);
  key("n", ID_K_NEXT);
  key("backspace", ID_KEY_PREV);
  key("p", ID_K_PREV);
  key("numericpad 7", ID_K_FIRST);
  key("numericpad 1", ID_K_LAST);
  key("numericpad 9", ID_K_PGUP);
  key("numericpad 3", ID_K_PGDN);
  key("numericpad +", ID_ZOOM_IN);
  key("shift =", ID_ZOOM_IN);
  key("=", ID_ZOOM_IN);
  key("-", ID_ZOOM_OUT);
  key("numericpad -", ID_ZOOM_OUT);
  key("w", ID_K_FITW);
  key("f", ID_K_FITP);
  key("g", ID_GOTO);
  key("b", ID_K_BOOKMARKS);
  key("o", ID_K_OPEN);
  key("q", ID_K_QUIT);
  key("esc", ID_K_ESC);
  key("up", ID_UP);
  key("down", ID_DOWN);
  key("left", ID_LEFT);
  key("right", ID_RIGHT);
  key("shift up", ID_PGUP);
  key("shift down", ID_PGDN);
  key("shift left", ID_SLEFT);
  key("shift right", ID_SRIGHT);
}

// ---- main loop --------------------------------------------------------------------

/* Keys typed into a text box - or moving through the bookmarks list - are
   that object's: a shortcut that also means something there (letters,
   Backspace, Space, the cursor keys) waits. */
static BOOL typing(void)
{
  ULONG a = xget(v.win, MUIA_Window_ActiveObject);
  return (a == (ULONG)v.sPage || a == (ULONG)v.sFind || a == (ULONG)v.lvBookmarks || a == (ULONG)v.lBookmarks)
           ? TRUE
           : FALSE;
}

static void scroll(LONG dx, LONG dy)
{
  DoMethod(v.view, MUIM_PageView_ScrollBy, dx, dy);
}

static void setCache(int idx)
{
  v.cacheIdx = idx;
  workerSetCache(cacheMB[idx] << 20);
  setVarInt("WarpPDFView.cache", (int)cacheMB[idx]);
}

static BOOL handle(ULONG id)
{
  char path[512];
  LONG vw = viewW(), vh = viewH();
  LONG step = vh / 8 + 1, pageStep = vh - vh / 10;

  // Keys that mean something in a text box belong to it while it is typed into.
  switch(id) {
    case ID_KEY_NEXT:
    case ID_KEY_PREV:
    case ID_K_NEXT:
    case ID_K_PREV:
    case ID_K_OPEN:
    case ID_K_FITW:
    case ID_K_FITP:
    case ID_K_FIRST:
    case ID_K_LAST:
    case ID_K_PGUP:
    case ID_K_PGDN:
    case ID_K_QUIT:
    case ID_K_BOOKMARKS:
    case ID_ZOOM_IN:
    case ID_ZOOM_OUT:
    case ID_GOTO:
    case ID_UP:
    case ID_DOWN:
    case ID_PGUP:
    case ID_PGDN:
    case ID_LEFT:
    case ID_RIGHT:
    case ID_SLEFT:
    case ID_SRIGHT:
      if(typing()) {
        return TRUE;
      }
      break;
  }
  // The letter keys do what their buttons and menu items do.
  switch(id) {
    case ID_K_NEXT:
      id = ID_NEXT;
      break;
    case ID_K_PREV:
      id = ID_PREV;
      break;
    case ID_K_OPEN:
      id = ID_OPEN;
      break;
    case ID_K_FITW:
      id = ID_FIT_WIDTH;
      break;
    case ID_K_FITP:
      id = ID_FIT_PAGE;
      break;
    case ID_K_FIRST:
      id = ID_FIRST;
      break;
    case ID_K_LAST:
      id = ID_LAST;
      break;
    case ID_K_PGUP:
      id = ID_PGUP;
      break;
    case ID_K_PGDN:
      id = ID_PGDN;
      break;
    case ID_K_QUIT:
      return FALSE;
    case ID_K_ESC:
      // Out of a text box, else quit.
      if(typing()) {
        set(v.win, MUIA_Window_ActiveObject, MUIV_Window_ActiveObject_None);
        return TRUE;
      }
      return FALSE;
  }

  switch(id) {
    case ID_OPEN:
      if(askFile(path, sizeof(path))) {
        openDocument(path);
      }
      break;
    case ID_ABOUT:
      MUI_Request(v.app, v.win, 0, (STRPTR) "About WarpPDFView", (STRPTR) "*_OK",
                  (STRPTR) "\33c" VERSION_STRING "\n\nA PDF viewer for Picasso96 screens\n"
                           "rendering with MuPDF 1.28.5 (c) Artifex Software\n\n"
                           "(c) CS-Lab s.c.\n\n"
                           "Free software under the GNU Affero General Public License v3.\n"
                           "There is NO WARRANTY. The complete source code,\n"
                           "including the MuPDF changes, is available at\n"
                           "\33b" SOURCE_URL "\33n");
      break;
    case ID_FIRST:
      gotoPage(0, FALSE);
      break;
    case ID_PREV:
      gotoPage(v.cur - 1, FALSE);
      break;
    case ID_NEXT:
      gotoPage(v.cur + 1, FALSE);
      break;
    case ID_LAST:
      gotoPage(v.count - 1, FALSE);
      break;
    case ID_KEY_NEXT:
      if(v.mode == MODE_CONT) {
        scroll(0, pageStep);
      } else {
        gotoPage(v.cur + 1, FALSE);
      }
      break;
    case ID_KEY_PREV:
      if(v.mode == MODE_CONT) {
        scroll(0, -pageStep);
      } else {
        gotoPage(v.cur - 1, FALSE);
      }
      break;
    case ID_PAGE:
      gotoPage((LONG)xget(v.sPage, MUIA_String_Integer) - 1, FALSE);
      set(v.win, MUIA_Window_ActiveObject, MUIV_Window_ActiveObject_None);
      break;
    case ID_GOTO:
      set(v.win, MUIA_Window_ActiveObject, (ULONG)v.sPage);
      break;
    case ID_ZOOM:
      setZoom((int)xget(v.cZoom, MUIA_Cycle_Active));
      break;
    case ID_AA:
      v.aa = (int)xget(v.cAA, MUIA_Cycle_Active);
      // Another anti-aliasing is another picture: like a new scale.
      unpinAll();
      v.scaleKey = 0;
      rescale();
      break;
    case ID_ZOOM_IN:
      stepZoom(1);
      break;
    case ID_ZOOM_OUT:
      stepZoom(-1);
      break;
    case ID_FIT_WIDTH:
      setZoom(ZOOM_FITWIDTH);
      break;
    case ID_FIT_PAGE:
      setZoom(ZOOM_FITPAGE);
      break;
    case ID_UP:
      scroll(0, -step);
      break;
    case ID_DOWN:
      scroll(0, step);
      break;
    case ID_PGUP:
      scroll(0, -pageStep);
      break;
    case ID_PGDN:
      scroll(0, pageStep);
      break;
    case ID_LEFT:
      scroll(-(vw / 8 + 1), 0);
      break;
    case ID_RIGHT:
      scroll(vw / 8 + 1, 0);
      break;
    case ID_SLEFT:
      scroll(-(vw - vw / 10), 0);
      break;
    case ID_SRIGHT:
      scroll(vw - vw / 10, 0);
      break;
    case ID_MODE_SINGLE:
      setMode(MODE_SINGLE);
      break;
    case ID_MODE_CONT:
      setMode(MODE_CONT);
      break;
    case ID_FIND:
      set(v.win, MUIA_Window_ActiveObject, (ULONG)v.sFind);
      break;
    case ID_FIND_NEXT:
      findStep(1);
      break;
    case ID_FIND_PREV:
      findStep(-1);
      break;
    case ID_CACHE_4:
    case ID_CACHE_8:
    case ID_CACHE_16:
    case ID_CACHE_32:
    case ID_CACHE_64:
      setCache((int)(id - ID_CACHE_4));
      break;
    case ID_BOOKMARKS: {
      // The menu has toggled its own check mark: follow it.
      Object *item = (Object *)DoMethod(v.strip, MUIM_FindUData, ID_BOOKMARKS);
      v.bmWanted = (item != NULL && xget(item, MUIA_Menuitem_Checked)) ? TRUE : FALSE;
      setVarInt("WarpPDFView.bookmarks", v.bmWanted ? 1 : 0);
      showBookmarks();
      if(v.open && !v.searching) {
        bookmarkStatus();
      }
      break;
    }
    case ID_K_BOOKMARKS:
      v.bmWanted = !v.bmWanted;
      DoMethod(v.strip, MUIM_SetUData, ID_BOOKMARKS, MUIA_Menuitem_Checked, (ULONG)v.bmWanted);
      setVarInt("WarpPDFView.bookmarks", v.bmWanted ? 1 : 0);
      showBookmarks();
      if(v.open && !v.searching) {
        bookmarkStatus();
      }
      break;
    case ID_BOOKMARK: {
      LONG idx = (LONG)xget(v.lBookmarks, MUIA_List_Active);
      if(idx >= 0) {
        gotoBookmark(idx);
      }
      break;
    }
  }
  return TRUE;
}

static void mainLoop(void)
{
  ULONG signals = 0;
  BOOL running = TRUE;

  while(running) {
    ULONG id = DoMethod(v.app, MUIM_Application_NewInput, (ULONG)&signals);

    if(id == (ULONG)MUIV_Application_ReturnID_Quit) {
      running = FALSE;
    } else if(id != 0) {
      running = handle(id);
    }
    if(running && signals) {
      if(Wait(signals | v.workerSig | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        running = FALSE;
      }
    }
    drainResults();
  }
}

// ---- start --------------------------------------------------------------------------

static BOOL openLibs(void)
{
  IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
  GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
  UtilityBase = OpenLibrary((STRPTR) "utility.library", 39);
  AslBase = OpenLibrary((STRPTR) "asl.library", 38);
  if(IntuitionBase == NULL || GfxBase == NULL || UtilityBase == NULL) {
    printf("WarpPDFView needs AmigaOS 3.0 or newer.\n");
    return FALSE;
  }
  MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19);
  if(MUIMasterBase == NULL) {
    printf("WarpPDFView needs MUI 3.8 or newer.\n");
    return FALSE;
  }
  P96Base = OpenLibrary((STRPTR) "Picasso96API.library", 2);
  if(P96Base == NULL) {
    printf("WarpPDFView needs Picasso96 (Picasso96API.library).\n");
    return FALSE;
  }
  return TRUE;
}

static void closeLibs(void)
{
  if(P96Base != NULL) {
    CloseLibrary(P96Base);
  }
  if(MUIMasterBase != NULL) {
    CloseLibrary(MUIMasterBase);
  }
  if(AslBase != NULL) {
    CloseLibrary(AslBase);
  }
  if(UtilityBase != NULL) {
    CloseLibrary(UtilityBase);
  }
  if(GfxBase != NULL) {
    CloseLibrary((struct Library *)GfxBase);
  }
  if(IntuitionBase != NULL) {
    CloseLibrary((struct Library *)IntuitionBase);
  }
}

enum { ARG_FILE, ARG_PAGE, ARG_DPI, ARG_AA, ARG_COUNT };
#define TEMPLATE "FILE,PAGE/N/K,DPI/N/K,AA/N/K"

int main(int argc, char **argv)
{
  LONG args[ARG_COUNT] = {0};
  struct RDArgs *rda = NULL;
  char path[512];
  int i, mb;
  int rc = RETURN_OK;
  BOOL workerUp = FALSE;

  (void)stackCookie;
  memset(&v, 0, sizeof(v));
  v.cur = 0;
  v.hitPage = -1;
  v.zoom = ZOOM_FITWIDTH;
  v.aa = 2;   // 4 bit: on screen as good as 8, and the drawing is ~28% cheaper
  path[0] = 0;

  if(!openLibs()) {
    closeLibs();
    return RETURN_FAIL;
  }
  v.mode = (getVarInt("WarpPDFView.mode", MODE_SINGLE) == MODE_CONT) ? MODE_CONT : MODE_SINGLE;
  mb = getVarInt("WarpPDFView.cache", 16);
  v.cacheIdx = 2;
  v.bmWanted = (getVarInt("WarpPDFView.bookmarks", 1) != 0) ? TRUE : FALSE;
  v.bmActive = -1;
  for(i = 0; i < CACHE_SIZES; i++) {
    if((int)cacheMB[i] == mb) {
      v.cacheIdx = i;
    }
  }

  if(argc == 0) {
    // From Workbench: a project icon's file is the second argument.
    struct WBStartup *wbs = (struct WBStartup *)argv;
    if(wbs->sm_NumArgs > 1 && wbs->sm_ArgList[1].wa_Lock != 0) {
      if(NameFromLock(wbs->sm_ArgList[1].wa_Lock, (STRPTR)path, sizeof(path))) {
        AddPart((STRPTR)path, wbs->sm_ArgList[1].wa_Name, sizeof(path));
      } else {
        path[0] = 0;
      }
    }
  } else {
    rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if(rda == NULL) {
      PrintFault(IoErr(), (STRPTR) "WarpPDFView");
      closeLibs();
      return RETURN_ERROR;
    }
    if(args[ARG_FILE] != 0) {
      strncpy(path, (const char *)args[ARG_FILE], sizeof(path) - 1);
      path[sizeof(path) - 1] = 0;
    }
    if(args[ARG_PAGE] != 0) {
      startPage = *(LONG *)args[ARG_PAGE] - 1;
    }
    if(args[ARG_DPI] != 0) {
      LONG dpi = *(LONG *)args[ARG_DPI];
      for(i = ZOOM_FITPAGE + 1; i < ZOOM_COUNT; i++) {
        if(zoomDpi[i] >= dpi) {
          break;
        }
      }
      v.zoom = (i < ZOOM_COUNT) ? i : ZOOM_COUNT - 1;
    }
    if(args[ARG_AA] != 0) {
      LONG bits = *(LONG *)args[ARG_AA];
      v.aa = (bits >= 8) ? 0 : (bits >= 6) ? 1 : (bits >= 4) ? 2 : 3;
    }
  }

  if(!createPageViewClass() || !buildApp()) {
    printf("WarpPDFView: cannot create the window (MUI).\n");
    rc = RETURN_FAIL;
  } else if(!workerStart(cacheMB[v.cacheIdx] << 20, &v.workerSig)) {
    message("Cannot start the render task (out of memory?).");
    rc = RETURN_FAIL;
  } else {
    workerUp = TRUE;
    notifications();
    set(v.win, MUIA_Window_Open, TRUE);
    if(!xget(v.win, MUIA_Window_Open)) {
      message("Cannot open the window.");
    } else if(!xget(v.view, MUIA_PageView_ScreenOk)) {
      message("WarpPDFView needs a Picasso96 screen\nwith 15 bits or more (true colour).");
    } else {
      set(v.win, MUIA_Window_DefaultObject, (ULONG)v.view);
      if(path[0] == 0) {
        askFile(path, sizeof(path));
      }
      if(path[0] != 0) {
        openDocument(path);
      } else {
        setStatus("No document - use Open");
      }
      mainLoop();
    }
    set(v.win, MUIA_Window_Open, FALSE);
  }
  /* The view lets go of the pages before the worker frees them, and the
     worker goes before its code does. */
  if(v.view != NULL) {
    closeDocument();
  }
  if(workerUp) {
    workerStop();
  }
  if(v.app != NULL) {
    MUI_DisposeObject(v.app);
  } else if(v.win != NULL) {
    MUI_DisposeObject(v.win);
  }
  deletePageViewClass();
  if(rda != NULL) {
    FreeArgs(rda);
  }
  closeLibs();
  return rc;
}

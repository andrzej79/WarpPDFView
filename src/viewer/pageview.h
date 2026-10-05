#ifndef PAGEVIEW_H
#define PAGEVIEW_H

/**
 * PageView: the MUI object that shows pages.
 *
 * It draws a layout the application hands it: an array of page slots, each a
 * rectangle on a canvas, with the page's rendered pixels or none yet.  Single
 * page mode is one slot, continuous mode one per page, stacked.
 *
 * A slot with pixels is uploaded once into a friend bitmap of the window's -
 * on the card, in the screen's format - and drawn with one blit from then on:
 * scrolling, exposes and window moves cost the card's blitter and nothing
 * else.  Only slots near the view keep a card bitmap.  If the card has no
 * room even then, the part in view comes straight from fast RAM.  A slot
 * without pixels is a white placeholder with its page number, drawn at once.
 *
 * Search hits are drawn as outlines over the page, the current one stronger.
 *
 * Two MUI Scrollbar objects are attached with MUIA_PageView_HScroller and
 * _VScroller; the view keeps them in step (without notification) and they
 * set MUIA_PageView_Left/_Top back through ordinary notifications.  Whatever
 * moves the view, and a flip past either end of a page (single page mode) or
 * a new size, goes to the application through MUIA_PageView_Callback.
 */

#include <exec/types.h>
#include <graphics/gfx.h>
#include <libraries/mui.h>
#include "worker.h"

typedef struct {
  LONG x0, y0, x1, y1;
} PvRect;

typedef struct {
  // Set by the application
  LONG x, y, w, h;           // the page on the canvas, in pixels
  LONG page;                 // 0-based, for the placeholder
  const PdfImage *img;       // NULL: not rendered (yet)
  ULONG serial;              // changed whenever img changes
  const PvRect *hits;        // search hits, in the page's pixels
  LONG hitCount;
  LONG hitActive;            // -1: none
  // The view's own
  struct BitMap *bm;
  ULONG bmSerial;
  BOOL bmFailed;
} PvSlot;

// Callback events
#define PVE_MOVED   1        // the view moved (scrollers, drag, keys, wheel)
#define PVE_FLIP    2        // value +1/-1: scrolled past the page's end/start
#define PVE_RESIZED 3        // the view changed size

typedef void (*PvCallback)(LONG event, LONG value);

#define PAGEVIEW_TAGBASE (TAG_USER | (1UL << 16) | 0x0600)

// Attributes
#define MUIA_PageView_Left        (PAGEVIEW_TAGBASE + 0x01)  // [SG.] LONG, scroll offset
#define MUIA_PageView_Top         (PAGEVIEW_TAGBASE + 0x02)  // [SG.] LONG
#define MUIA_PageView_HScroller   (PAGEVIEW_TAGBASE + 0x03)  // [I..] Object *
#define MUIA_PageView_VScroller   (PAGEVIEW_TAGBASE + 0x04)  // [I..] Object *
#define MUIA_PageView_ViewWidth   (PAGEVIEW_TAGBASE + 0x05)  // [..G] LONG, 0 until shown
#define MUIA_PageView_ViewHeight  (PAGEVIEW_TAGBASE + 0x06)  // [..G] LONG
#define MUIA_PageView_ScreenOk    (PAGEVIEW_TAGBASE + 0x07)  // [..G] BOOL, a P96 screen of 15 bpp+
#define MUIA_PageView_Callback    (PAGEVIEW_TAGBASE + 0x08)  // [I..] PvCallback
#define MUIA_PageView_FlipAtEnds  (PAGEVIEW_TAGBASE + 0x09)  // [IS.] BOOL: wheel/keys past an end flip

// Methods
#define MUIM_PageView_SetLayout   (PAGEVIEW_TAGBASE + 0x20)  // (PvSlot *, n, canvasW, canvasH, keep)
#define MUIM_PageView_SlotChanged (PAGEVIEW_TAGBASE + 0x21)  // (LONG index): img or hits changed
#define MUIM_PageView_ScrollBy    (PAGEVIEW_TAGBASE + 0x22)  // (LONG dx, LONG dy) -> BOOL moved
#define MUIM_PageView_Deferred    (PAGEVIEW_TAGBASE + 0x23)  // internal

struct MUIP_PageView_SetLayout {
  ULONG MethodID;
  PvSlot *slots;
  LONG count;
  LONG canvasW, canvasH;
  LONG keep;   // TRUE: same slots, same pages, only moved - keep their card bitmaps
};

struct MUIP_PageView_SlotChanged {
  ULONG MethodID;
  LONG index;
};

struct MUIP_PageView_ScrollBy {
  ULONG MethodID;
  LONG dx, dy;
};

extern struct MUI_CustomClass *PageViewClass;

BOOL createPageViewClass(void);
void deletePageViewClass(void);

#define PageViewObject NewObject(PageViewClass->mcc_Class, NULL

#endif

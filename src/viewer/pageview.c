/**
 * PageView - see pageview.h.
 */

#ifndef __VBCC__
#define __reg(x)
#endif

#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>

/* Picasso96: the structures from the P96 developer includes (P96_SDK in the
   Makefile), the vbcc call stubs from the compiler's own tree. */
#include <libraries/Picasso96.h>
#pragma stdargs-on
#include <clib/Picasso96_protos.h>
#pragma stdargs-off
#include <inline/Picasso96_protos.h>

#include "pageview.h"

extern struct Library *P96Base;

struct MUI_CustomClass *PageViewClass = NULL;

#define RK_WHEELUP   0x7A    // NewMouse wheel
#define RK_WHEELDOWN 0x7B

enum { PEN_WHITE, PEN_BORDER, PEN_LABEL, PEN_HIT, PEN_ACTIVE, PEN_LINES, PENS };

struct PageViewData {
  PvSlot *slots;
  LONG count;
  LONG canvasW, canvasH;

  LONG left, top;            // scroll offsets into the canvas
  LONG lastW, lastH;         // the view's size at the last draw, 0 before the first
  Object *hbar, *vbar;
  PvCallback callback;
  BOOL flipAtEnds;

  BOOL screenOk;
  BOOL shown;
  BOOL pensOk;
  LONG pens[PENS];
  BOOL pensOwned[PENS];

  struct MUI_EventHandlerNode ehn;
  BOOL ehnAdded;
  BOOL dragging;
  BOOL hadReportMouse;
  LONG dragMx, dragMy, dragLeft, dragTop;
};

// ---- helpers ------------------------------------------------------------------

static BOOL screenSupported(struct Screen *scr)
{
  if(P96Base == NULL || scr == NULL) {
    return FALSE;
  }
  if(!p96GetBitMapAttr(scr->RastPort.BitMap, P96BMA_ISP96)) {
    return FALSE;
  }
  return (p96GetBitMapAttr(scr->RastPort.BitMap, P96BMA_DEPTH) >= 15) ? TRUE : FALSE;
}

static void freeSlotBitMap(PvSlot *s)
{
  if(s->bm != NULL) {
    // The card may still be reading from it: WaitBlit() before FreeBitMap().
    WaitBlit();
    FreeBitMap(s->bm);
    s->bm = NULL;
  }
}

static void freeAllBitMaps(struct PageViewData *d)
{
  LONG i;

  for(i = 0; i < d->count; i++) {
    freeSlotBitMap(&d->slots[i]);
    d->slots[i].bmFailed = FALSE;
  }
}

static LONG viewW(Object *obj, struct PageViewData *d)
{
  return d->shown ? _mwidth(obj) : 0;
}

static LONG viewH(Object *obj, struct PageViewData *d)
{
  return d->shown ? _mheight(obj) : 0;
}

static void clampScroll(Object *obj, struct PageViewData *d)
{
  LONG vw = viewW(obj, d), vh = viewH(obj, d);
  LONG maxX = (d->canvasW > vw) ? d->canvasW - vw : 0;
  LONG maxY = (d->canvasH > vh) ? d->canvasH - vh : 0;

  if(d->left > maxX) {
    d->left = maxX;
  }
  if(d->top > maxY) {
    d->top = maxY;
  }
  if(d->left < 0) {
    d->left = 0;
  }
  if(d->top < 0) {
    d->top = 0;
  }
}

/* The canvas's origin in the view: centred when it is smaller, scrolled when
   it is larger. */
static void origin(Object *obj, struct PageViewData *d, LONG *ox, LONG *oy)
{
  LONG vw = viewW(obj, d), vh = viewH(obj, d);

  *ox = (d->canvasW <= vw) ? (vw - d->canvasW) / 2 : -d->left;
  *oy = (d->canvasH <= vh) ? (vh - d->canvasH) / 2 : -d->top;
}

/* The scrollers follow the view without notifying back. */
static void updateBars(Object *obj, struct PageViewData *d)
{
  LONG vw = viewW(obj, d), vh = viewH(obj, d);
  LONG tw = (d->canvasW > 0) ? d->canvasW : 1, th = (d->canvasH > 0) ? d->canvasH : 1;

  if(vw < 1) {
    vw = 1;
  }
  if(vh < 1) {
    vh = 1;
  }
  if(d->hbar != NULL) {
    SetAttrs(d->hbar, MUIA_NoNotify, TRUE, MUIA_Prop_Entries, tw, MUIA_Prop_Visible, vw, MUIA_Prop_First, d->left,
             TAG_DONE);
  }
  if(d->vbar != NULL) {
    SetAttrs(d->vbar, MUIA_NoNotify, TRUE, MUIA_Prop_Entries, th, MUIA_Prop_Visible, vh, MUIA_Prop_First, d->top,
             TAG_DONE);
  }
}

static void redraw(Object *obj, struct PageViewData *d)
{
  if(d->shown) {
    MUI_Redraw(obj, MADF_DRAWUPDATE);
  }
}

static void callback(struct PageViewData *d, LONG event, LONG value)
{
  if(d->callback != NULL) {
    d->callback(event, value);
  }
}

// After the view moved by any means: scrollers, picture, application.
static void moved(Object *obj, struct PageViewData *d)
{
  updateBars(obj, d);
  redraw(obj, d);
  callback(d, PVE_MOVED, 0);
}

static BOOL scrollBy(Object *obj, struct PageViewData *d, LONG dx, LONG dy)
{
  LONG ol = d->left, ot = d->top, vh = viewH(obj, d);
  LONG maxY = (d->canvasH > vh) ? d->canvasH - vh : 0;

  if(d->flipAtEnds) {
    if(dy > 0 && d->top >= maxY) {
      callback(d, PVE_FLIP, 1);
      return FALSE;
    }
    if(dy < 0 && d->top <= 0) {
      callback(d, PVE_FLIP, -1);
      return FALSE;
    }
  }
  d->left += dx;
  d->top += dy;
  clampScroll(obj, d);
  if(d->left == ol && d->top == ot) {
    return FALSE;
  }
  moved(obj, d);
  return TRUE;
}

static void setEvents(Object *obj, struct PageViewData *d, ULONG events)
{
  if(d->ehnAdded) {
    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
  }
  d->ehn.ehn_Events = events;
  DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
  d->ehnAdded = TRUE;
}

static void endDrag(Object *obj, struct PageViewData *d)
{
  struct Window *w = _window(obj);

  if(!d->dragging) {
    return;
  }
  d->dragging = FALSE;
  if(w != NULL && !d->hadReportMouse) {
    ReportMouse(FALSE, w);
  }
  setEvents(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY);
}

// ---- drawing ------------------------------------------------------------------

static void drawBackground(Object *obj, LONG x, LONG y, LONG w, LONG h)
{
  if(w > 0 && h > 0) {
    DoMethod(obj, MUIM_DrawBackground, x, y, w, h, 0, 0, 0);
  }
}

static void fill(struct RastPort *rp, LONG pen, LONG x0, LONG y0, LONG x1, LONG y1)
{
  // [x0, x1) x [y0, y1)
  if(x1 > x0 && y1 > y0) {
    SetAPen(rp, pen);
    RectFill(rp, x0, y0, x1 - 1, y1 - 1);
  }
}

// A rectangle's outline, t pixels wide, drawn only inside the clip.
static void outline(struct RastPort *rp, LONG pen, LONG x0, LONG y0, LONG x1, LONG y1, LONG t, const PvRect *c)
{
  PvRect e[4];
  int i;

  e[0].x0 = x0; e[0].y0 = y0; e[0].x1 = x1; e[0].y1 = y0 + t;
  e[1].x0 = x0; e[1].y0 = y1 - t; e[1].x1 = x1; e[1].y1 = y1;
  e[2].x0 = x0; e[2].y0 = y0; e[2].x1 = x0 + t; e[2].y1 = y1;
  e[3].x0 = x1 - t; e[3].y0 = y0; e[3].x1 = x1; e[3].y1 = y1;
  for(i = 0; i < 4; i++) {
    fill(rp, pen, (e[i].x0 > c->x0) ? e[i].x0 : c->x0, (e[i].y0 > c->y0) ? e[i].y0 : c->y0,
         (e[i].x1 < c->x1) ? e[i].x1 : c->x1, (e[i].y1 < c->y1) ? e[i].y1 : c->y1);
  }
}

static BOOL slotVisible(Object *obj, struct PageViewData *d, const PvSlot *s)
{
  LONG ox, oy, vw = viewW(obj, d), vh = viewH(obj, d);
  LONG x0, y0;

  origin(obj, d, &ox, &oy);
  x0 = ox + s->x;
  y0 = oy + s->y;
  return (x0 < vw && x0 + s->w > 0 && y0 < vh && y0 + s->h > 0) ? TRUE : FALSE;
}

// Card bitmaps of every slot out of view: the card is short.
static void freeInvisible(Object *obj, struct PageViewData *d, const PvSlot *keep)
{
  LONG i;

  for(i = 0; i < d->count; i++) {
    PvSlot *s = &d->slots[i];
    if(s != keep && s->bm != NULL && !slotVisible(obj, d, s)) {
      freeSlotBitMap(s);
    }
  }
}

/* The page on the card, uploaded once: a friend of the window's bitmap, so
   it is in the screen's format and the blits are straight copies. */
static void upload(Object *obj, struct PageViewData *d, PvSlot *s)
{
  struct RenderInfo ri;
  struct RastPort rp;
  ULONG depth = GetBitMapAttr(_rp(obj)->BitMap, BMA_DEPTH);

  freeSlotBitMap(s);
  s->bmSerial = s->serial;
  s->bmFailed = FALSE;
  s->bm = AllocBitMap((ULONG)s->img->w, (ULONG)s->img->h, depth, BMF_MINPLANES, _rp(obj)->BitMap);
  if(s->bm == NULL) {
    freeInvisible(obj, d, s);
    s->bm = AllocBitMap((ULONG)s->img->w, (ULONG)s->img->h, depth, BMF_MINPLANES, _rp(obj)->BitMap);
  }
  if(s->bm == NULL) {
    s->bmFailed = TRUE;
    return;
  }
  InitRastPort(&rp);
  rp.BitMap = s->bm;
  ri.Memory = s->img->pixels;
  ri.BytesPerRow = (WORD)s->img->stride;
  ri.pad = 0;
  ri.RGBFormat = RGBFB_R8G8B8;
  p96WritePixelArray(&ri, 0, 0, &rp, 0, 0, (UWORD)s->img->w, (UWORD)s->img->h);
}

/* A placeholder's imitation text: a heading, then paragraphs of grey bars,
   each ending with a short line.  The lengths come from the page number, so a
   page looks the same every time it is drawn and pages differ from each
   other.  The rows round the page-number label (ly0..ly1) stay clear. */
static void drawTextLines(struct RastPort *rp, LONG pen, const PvSlot *s, const PvRect *c, LONG px, LONG py,
                          LONG ly0, LONG ly1)
{
  ULONG seed = (ULONG)s->page * 2654435761UL + 12345UL;
  LONG mx = s->w / 10, left = px + mx, width = s->w - 2 * mx;
  LONG lh = s->h / 110, pitch, y, bottom = py + s->h - s->h / 12, inPara = 0, paraLen = 0;

  if(lh < 2) {
    lh = 2;
  }
  pitch = lh * 2 + lh / 2;
  if(width < 8 || s->h < 40) {
    return;
  }
  // The heading: taller, about half the width.
  y = py + s->h / 12;
  if(y + lh * 2 < ly0 || y > ly1) {
    LONG hx1 = left + width / 2 + (LONG)(seed % (ULONG)(width / 4 + 1));
    fill(rp, pen, (left > c->x0) ? left : c->x0, (y > c->y0) ? y : c->y0, (hx1 < c->x1) ? hx1 : c->x1,
         (y + lh * 2 < c->y1) ? y + lh * 2 : c->y1);
  }
  y += lh * 5;
  while(y + lh <= bottom) {
    LONG len;
    seed = seed * 1103515245UL + 12345UL;
    if(inPara == 0) {
      paraLen = 4 + (LONG)((seed >> 16) % 6);   // 4..9 lines
    }
    inPara++;
    if(inPara == paraLen) {
      len = width / 4 + (LONG)((seed >> 8) % (ULONG)(width / 2 + 1));   // the paragraph's short last line
    } else {
      len = width - (LONG)((seed >> 8) % (ULONG)(width / 12 + 1));
    }
    if((y + lh < ly0 || y > ly1) && y + lh > c->y0 && y < c->y1) {
      LONG x0 = (left > c->x0) ? left : c->x0;
      LONG x1 = (left + len < c->x1) ? left + len : c->x1;
      LONG y0 = (y > c->y0) ? y : c->y0;
      LONG y1 = (y + lh < c->y1) ? y + lh : c->y1;
      fill(rp, pen, x0, y0, x1, y1);
    }
    y += pitch;
    if(inPara == paraLen) {
      // A blank line between paragraphs.
      inPara = 0;
      y += pitch;
    }
  }
}

/* One slot: the page (or its placeholder) inside clip c, the page's top left
   on screen at (px, py). */
static void drawSlot(Object *obj, struct PageViewData *d, PvSlot *s, const PvRect *c, LONG px, LONG py)
{
  struct RastPort *rp = _rp(obj);
  LONG i;

  if(s->img != NULL) {
    LONG iw = s->img->w, ih = s->img->h;
    LONG sx = c->x0 - px, sy = c->y0 - py;
    LONG w = c->x1 - c->x0, h = c->y1 - c->y0;

    if(s->bmSerial != s->serial || (s->bm == NULL && !s->bmFailed)) {
      upload(obj, d, s);
    }
    // The pixels cover [0, iw) x [0, ih) of the slot; a rounding pixel short is white.
    if(sx + w > iw) {
      fill(rp, d->pens[PEN_WHITE], px + iw, c->y0, c->x1, c->y1);
      w = iw - sx;
    }
    if(sy + h > ih) {
      fill(rp, d->pens[PEN_WHITE], c->x0, py + ih, c->x0 + ((w > 0) ? w : 0), c->y1);
      h = ih - sy;
    }
    if(w > 0 && h > 0) {
      if(s->bm != NULL) {
        BltBitMapRastPort(s->bm, sx, sy, rp, c->x0, c->y0, w, h, 0xC0);
      } else {
        struct RenderInfo ri;
        ri.Memory = s->img->pixels;
        ri.BytesPerRow = (WORD)s->img->stride;
        ri.pad = 0;
        ri.RGBFormat = RGBFB_R8G8B8;
        p96WritePixelArray(&ri, (UWORD)sx, (UWORD)sy, rp, (UWORD)c->x0, (UWORD)c->y0, (UWORD)w, (UWORD)h);
      }
    }
  } else {
    // Not rendered yet: white, a thin border, grey lines for text, the page number.
    char label[24];
    LONG len, tw, th, tx, ty;
    fill(rp, d->pens[PEN_WHITE], c->x0, c->y0, c->x1, c->y1);
    outline(rp, d->pens[PEN_BORDER], px, py, px + s->w, py + s->h, 1, c);
    snprintf(label, sizeof(label), "Page %ld", (long)(s->page + 1));
    len = (LONG)strlen(label);
    SetFont(rp, _font(obj));
    tw = TextLength(rp, (STRPTR)label, len);
    th = rp->TxHeight;
    tx = px + (s->w - tw) / 2;
    ty = py + (s->h - th) / 2;
    drawTextLines(rp, d->pens[PEN_LINES], s, c, px, py, ty - th, ty + th * 2);
    if(tx >= c->x0 && tx + tw <= c->x1 && ty >= c->y0 && ty + th <= c->y1) {
      SetAPen(rp, d->pens[PEN_LABEL]);
      SetDrMd(rp, JAM1);
      Move(rp, tx, ty + rp->TxBaseline);
      Text(rp, (STRPTR)label, len);
    }
  }

  for(i = 0; i < s->hitCount; i++) {
    const PvRect *h = &s->hits[i];
    BOOL active = (i == s->hitActive) ? TRUE : FALSE;
    LONG m = active ? 3 : 2;
    outline(rp, d->pens[active ? PEN_ACTIVE : PEN_HIT], px + h->x0 - m, py + h->y0 - m, px + h->x1 + m,
            py + h->y1 + m, m, c);
  }
}

static ULONG pvDraw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
  struct PageViewData *d = INST_DATA(cl, obj);
  LONG x0, y0, vw, vh, ox, oy, yCursor, i;

  DoSuperMethodA(cl, obj, (Msg)msg);
  if(!(msg->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))) {
    return 0;
  }
  x0 = _mleft(obj);
  y0 = _mtop(obj);
  vw = _mwidth(obj);
  vh = _mheight(obj);
  if(vw != d->lastW || vh != d->lastH) {
    /* A new size.  The application refits the page if it zooms to fit; it
       hears of it after this draw, not inside it. */
    if(d->lastW != 0) {
      DoMethod(_app(obj), MUIM_Application_PushMethod, (ULONG)obj, 1, MUIM_PageView_Deferred);
    }
    d->lastW = vw;
    d->lastH = vh;
    clampScroll(obj, d);
  }
  if(!d->screenOk || d->slots == NULL || d->count == 0) {
    drawBackground(obj, x0, y0, vw, vh);
    return 0;
  }
  origin(obj, d, &ox, &oy);

  /* Slots are stacked top to bottom without overlapping: walk down the view,
     background in the gaps and at the sides, pages where they are. */
  yCursor = y0;
  for(i = 0; i < d->count; i++) {
    PvSlot *s = &d->slots[i];
    LONG sx0 = x0 + ox + s->x, sy0 = y0 + oy + s->y;
    LONG sx1 = sx0 + s->w, sy1 = sy0 + s->h;
    PvRect c;

    if(sy1 <= y0) {
      continue;
    }
    if(sy0 >= y0 + vh) {
      break;
    }
    c.y0 = (sy0 > y0) ? sy0 : y0;
    c.y1 = (sy1 < y0 + vh) ? sy1 : y0 + vh;
    if(c.y0 > yCursor) {
      drawBackground(obj, x0, yCursor, vw, c.y0 - yCursor);
    }
    c.x0 = (sx0 > x0) ? sx0 : x0;
    c.x1 = (sx1 < x0 + vw) ? sx1 : x0 + vw;
    if(c.x0 > x0) {
      drawBackground(obj, x0, c.y0, c.x0 - x0, c.y1 - c.y0);
    }
    if(c.x1 < x0 + vw) {
      drawBackground(obj, c.x1, c.y0, x0 + vw - c.x1, c.y1 - c.y0);
    }
    if(c.x1 > c.x0) {
      drawSlot(obj, d, s, &c, sx0, sy0);
    }
    yCursor = c.y1;
  }
  if(yCursor < y0 + vh) {
    drawBackground(obj, x0, yCursor, vw, y0 + vh - yCursor);
  }
  // Keep the card for what is on screen.
  freeInvisible(obj, d, NULL);
  return 0;
}

// ---- input -------------------------------------------------------------------

static ULONG pvHandleEvent(Object *obj, struct PageViewData *d, struct MUIP_HandleEvent *msg)
{
  struct IntuiMessage *im = msg->imsg;
  struct Window *w = _window(obj);
  LONG mx, my;
  BOOL inside;

  if(im == NULL || w == NULL) {
    return 0;
  }
  mx = im->MouseX;
  my = im->MouseY;
  inside = (mx >= _mleft(obj) && mx <= _mright(obj) && my >= _mtop(obj) && my <= _mbottom(obj)) ? TRUE : FALSE;

  switch(im->Class) {
    case IDCMP_MOUSEBUTTONS:
      if(im->Code == SELECTDOWN && inside && d->count > 0 && !d->dragging) {
        d->dragging = TRUE;
        d->dragMx = mx;
        d->dragMy = my;
        d->dragLeft = d->left;
        d->dragTop = d->top;
        d->hadReportMouse = (w->Flags & WFLG_REPORTMOUSE) ? TRUE : FALSE;
        if(!d->hadReportMouse) {
          ReportMouse(TRUE, w);
        }
        setEvents(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY);
        return MUI_EventHandlerRC_Eat;
      }
      if(im->Code == SELECTUP && d->dragging) {
        endDrag(obj, d);
        return MUI_EventHandlerRC_Eat;
      }
      return 0;

    case IDCMP_MOUSEMOVE:
      if(d->dragging) {
        LONG ol = d->left, ot = d->top;
        d->left = d->dragLeft - (mx - d->dragMx);
        d->top = d->dragTop - (my - d->dragMy);
        clampScroll(obj, d);
        if(d->left != ol || d->top != ot) {
          moved(obj, d);
        }
        return MUI_EventHandlerRC_Eat;
      }
      return 0;

    case IDCMP_RAWKEY:
      if(inside && (im->Code == RK_WHEELUP || im->Code == RK_WHEELDOWN)) {
        LONG step = viewH(obj, d) / 8 + 1;
        scrollBy(obj, d, 0, (im->Code == RK_WHEELUP) ? -step : step);
        return MUI_EventHandlerRC_Eat;
      }
      return 0;
  }
  return 0;
}

// ---- methods ------------------------------------------------------------------

static void parseTags(Object *obj, struct PageViewData *d, struct TagItem *tags, BOOL init)
{
  struct TagItem *t, *tp = tags;
  BOOL move = FALSE;

  while((t = NextTagItem(&tp)) != NULL) {
    switch(t->ti_Tag) {
      case MUIA_PageView_Left:
        if((LONG)t->ti_Data != d->left) {
          d->left = (LONG)t->ti_Data;
          move = TRUE;
        }
        break;
      case MUIA_PageView_Top:
        if((LONG)t->ti_Data != d->top) {
          d->top = (LONG)t->ti_Data;
          move = TRUE;
        }
        break;
      case MUIA_PageView_FlipAtEnds:
        d->flipAtEnds = t->ti_Data ? TRUE : FALSE;
        break;
      case MUIA_PageView_HScroller:
        if(init) {
          d->hbar = (Object *)t->ti_Data;
        }
        break;
      case MUIA_PageView_VScroller:
        if(init) {
          d->vbar = (Object *)t->ti_Data;
        }
        break;
      case MUIA_PageView_Callback:
        if(init) {
          d->callback = (PvCallback)t->ti_Data;
        }
        break;
    }
  }
  if(move && !init) {
    clampScroll(obj, d);
    moved(obj, d);
  }
}

static void pensGet(Object *obj, struct PageViewData *d)
{
  struct ColorMap *cm = _screen(obj)->ViewPort.ColorMap;
  static const ULONG rgb[PENS][3] = {
    { 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL },   // page white
    { 0x90909090UL, 0x90909090UL, 0x90909090UL },   // placeholder border
    { 0x70707070UL, 0x70707070UL, 0x70707070UL },   // placeholder label
    { 0xFFFFFFFFUL, 0x99999999UL, 0x00000000UL },   // search hit
    { 0xEEEEEEEEUL, 0x22222222UL, 0x22222222UL },   // the current hit
    { 0xE4E4E4E4UL, 0xE4E4E4E4UL, 0xE4E4E4E4UL },   // placeholder text lines
  };
  int i;

  for(i = 0; i < PENS; i++) {
    d->pens[i] = ObtainBestPen(cm, rgb[i][0], rgb[i][1], rgb[i][2], OBP_Precision, PRECISION_EXACT, TAG_DONE);
    d->pensOwned[i] = (d->pens[i] >= 0) ? TRUE : FALSE;
    if(d->pens[i] < 0) {
      // No pen to spare: the screen's own, white for the page, black for the rest.
      d->pens[i] = (i == PEN_WHITE) ? 2 : 1;
    }
  }
  d->pensOk = TRUE;
}

static void pensRelease(Object *obj, struct PageViewData *d)
{
  struct ColorMap *cm = _screen(obj)->ViewPort.ColorMap;
  int i;

  if(!d->pensOk) {
    return;
  }
  for(i = 0; i < PENS; i++) {
    if(d->pensOwned[i]) {
      ReleasePen(cm, (ULONG)d->pens[i]);
    }
  }
  d->pensOk = FALSE;
}

static ULONG pvDispatcher(__reg("a0") struct IClass *cl, __reg("a2") Object *obj, __reg("a1") Msg msg)
{
  struct PageViewData *d;

  switch(msg->MethodID) {
    case OM_NEW:
      if((obj = (Object *)DoSuperMethodA(cl, obj, msg)) == NULL) {
        return 0;
      }
      d = INST_DATA(cl, obj);
      memset(d, 0, sizeof(*d));
      d->flipAtEnds = TRUE;
      parseTags(obj, d, ((struct opSet *)msg)->ops_AttrList, TRUE);
      return (ULONG)obj;

    case OM_DISPOSE:
      d = INST_DATA(cl, obj);
      freeAllBitMaps(d);
      break;

    case OM_SET:
      d = INST_DATA(cl, obj);
      parseTags(obj, d, ((struct opSet *)msg)->ops_AttrList, FALSE);
      break;

    case OM_GET: {
      struct opGet *g = (struct opGet *)msg;
      d = INST_DATA(cl, obj);
      switch(g->opg_AttrID) {
        case MUIA_PageView_Left:
          *g->opg_Storage = (ULONG)d->left;
          return TRUE;
        case MUIA_PageView_Top:
          *g->opg_Storage = (ULONG)d->top;
          return TRUE;
        case MUIA_PageView_ViewWidth:
          *g->opg_Storage = (ULONG)viewW(obj, d);
          return TRUE;
        case MUIA_PageView_ViewHeight:
          *g->opg_Storage = (ULONG)viewH(obj, d);
          return TRUE;
        case MUIA_PageView_ScreenOk:
          *g->opg_Storage = (ULONG)d->screenOk;
          return TRUE;
      }
      break;
    }

    case MUIM_AskMinMax: {
      struct MUI_MinMax *mm;
      // The superclass first: MUI hands MinMaxInfo over only after that.
      DoSuperMethodA(cl, obj, msg);
      mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
      mm->MinWidth += 64;
      mm->MinHeight += 64;
      mm->DefWidth += 600;
      mm->DefHeight += 800;
      mm->MaxWidth += MUI_MAXMAX;
      mm->MaxHeight += MUI_MAXMAX;
      return 0;
    }

    case MUIM_Setup:
      if(!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
      }
      d = INST_DATA(cl, obj);
      d->screenOk = screenSupported(_screen(obj));
      pensGet(obj, d);
      d->ehn.ehn_Object = obj;
      d->ehn.ehn_Class = cl;
      d->ehn.ehn_Priority = 0;
      d->ehn.ehn_Flags = 0;
      d->ehnAdded = FALSE;
      setEvents(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY);
      return TRUE;

    case MUIM_Cleanup:
      d = INST_DATA(cl, obj);
      if(d->ehnAdded) {
        endDrag(obj, d);
        DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
        d->ehnAdded = FALSE;
      }
      /* The screen may change before the next Setup: the card copies go,
         the pixels in fast RAM stay and are uploaded again when needed. */
      freeAllBitMaps(d);
      pensRelease(obj, d);
      break;

    case MUIM_Show:
      if(!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
      }
      d = INST_DATA(cl, obj);
      d->shown = TRUE;
      return TRUE;

    case MUIM_Hide:
      d = INST_DATA(cl, obj);
      // lastW/lastH stay: a resize is a Hide and a Show, and must read as one.
      d->shown = FALSE;
      break;

    case MUIM_Draw:
      return pvDraw(cl, obj, (struct MUIP_Draw *)msg);

    case MUIM_HandleEvent:
      d = INST_DATA(cl, obj);
      return pvHandleEvent(obj, d, (struct MUIP_HandleEvent *)msg);

    case MUIM_PageView_SetLayout: {
      struct MUIP_PageView_SetLayout *m = (struct MUIP_PageView_SetLayout *)msg;
      d = INST_DATA(cl, obj);
      if(!m->keep || m->slots != d->slots) {
        freeAllBitMaps(d);
      }
      d->slots = m->slots;
      d->count = m->count;
      d->canvasW = m->canvasW;
      d->canvasH = m->canvasH;
      clampScroll(obj, d);
      updateBars(obj, d);
      redraw(obj, d);
      return 0;
    }

    case MUIM_PageView_SlotChanged: {
      struct MUIP_PageView_SlotChanged *m = (struct MUIP_PageView_SlotChanged *)msg;
      d = INST_DATA(cl, obj);
      if(m->index >= 0 && m->index < d->count) {
        PvSlot *s = &d->slots[m->index];
        if(s->bmSerial != s->serial) {
          freeSlotBitMap(s);
          s->bmFailed = FALSE;
        }
        if(d->shown && slotVisible(obj, d, s)) {
          redraw(obj, d);
        }
      }
      return 0;
    }

    case MUIM_PageView_ScrollBy: {
      struct MUIP_PageView_ScrollBy *m = (struct MUIP_PageView_ScrollBy *)msg;
      d = INST_DATA(cl, obj);
      if(m->dx == 0 && m->dy == 0) {
        // Only bring the scrollers in step.
        clampScroll(obj, d);
        updateBars(obj, d);
        redraw(obj, d);
        return FALSE;
      }
      return (ULONG)scrollBy(obj, d, m->dx, m->dy);
    }

    case MUIM_PageView_Deferred:
      d = INST_DATA(cl, obj);
      clampScroll(obj, d);
      updateBars(obj, d);
      callback(d, PVE_RESIZED, 0);
      return 0;
  }
  return DoSuperMethodA(cl, obj, msg);
}

BOOL createPageViewClass(void)
{
  PageViewClass = MUI_CreateCustomClass(NULL, (STRPTR)MUIC_Area, NULL, sizeof(struct PageViewData), (APTR)pvDispatcher);
  return (PageViewClass != NULL) ? TRUE : FALSE;
}

void deletePageViewClass(void)
{
  if(PageViewClass != NULL) {
    MUI_DeleteCustomClass(PageViewClass);
    PageViewClass = NULL;
  }
}

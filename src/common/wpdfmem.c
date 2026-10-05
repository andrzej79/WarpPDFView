/**
 * MuPDF's heap - see wpdfmem.h.
 */

#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <proto/exec.h>

#include "wpdfmem.h"

#define GRANULE    16          // small block sizes are multiples of this
#define SMALL_MAX  2048        // largest small block, header included
#define NCLASSES   (SMALL_MAX / GRANULE + 1)
#define CHUNK_SIZE 65536
#define LARGE      0           // class of a block that came from AllocVec()

/* In front of every block.  A small one's header sits directly before the
   payload; a large one has a list node before that, for wpdfMemCleanup(). */
typedef struct {
  ULONG size;                  // bytes asked for
  ULONG cls;                   // size class, LARGE for AllocVec() blocks
} BlockHdr;

typedef struct {
  struct MinNode node;
  BlockHdr hdr;
} LargeHdr;

typedef struct FreeBlock {
  struct FreeBlock *next;
} FreeBlock;

static FreeBlock *freeList[NCLASSES];
static UBYTE *chunks = NULL;   // chained through their first word
static UBYTE *carve = NULL;    // unused part of the newest chunk
static UBYTE *carveEnd = NULL;
static struct MinList large;
static BOOL largeInit = FALSE;

static size_t inUse = 0;
static size_t peak = 0;
static unsigned long osCalls = 0;

static void account(long delta)
{
  inUse += delta;
  if(inUse > peak) {
    peak = inUse;
  }
}

static void *allocLarge(size_t size)
{
  LargeHdr *h;

  if(!largeInit) {
    NewList((struct List *)&large);
    largeInit = TRUE;
  }
  h = (LargeHdr *)AllocVec(sizeof(LargeHdr) + size, MEMF_ANY);
  osCalls++;
  if(h == NULL) {
    return NULL;
  }
  AddTail((struct List *)&large, (struct Node *)&h->node);
  h->hdr.size = size;
  h->hdr.cls = LARGE;
  account((long)size);
  return h + 1;
}

static void *allocSmall(size_t size, ULONG cls)
{
  BlockHdr *h;
  FreeBlock *f = freeList[cls];

  if(f != NULL) {
    freeList[cls] = f->next;
    h = (BlockHdr *)f;
  } else {
    ULONG bytes = cls * GRANULE;
    if(carve == NULL || (ULONG)(carveEnd - carve) < bytes) {
      /* What is left of the old chunk (under SMALL_MAX) is abandoned. */
      UBYTE *c = (UBYTE *)AllocVec(CHUNK_SIZE, MEMF_ANY);
      osCalls++;
      if(c == NULL) {
        return NULL;
      }
      *(UBYTE **)c = chunks;
      chunks = c;
      carve = c + 8;           // keeps payloads 8-byte aligned
      carveEnd = c + CHUNK_SIZE;
    }
    h = (BlockHdr *)carve;
    carve += bytes;
  }
  h->size = size;
  h->cls = cls;
  account((long)size);
  return h + 1;
}

void *wpdfMemAlloc(size_t size)
{
  size_t total = size + sizeof(BlockHdr);

  if(total > SMALL_MAX) {
    return allocLarge(size);
  }
  return allocSmall(size, (ULONG)((total + GRANULE - 1) / GRANULE));
}

void wpdfMemFree(void *ptr)
{
  BlockHdr *h;

  if(ptr == NULL) {
    return;
  }
  h = (BlockHdr *)ptr - 1;
  account(-(long)h->size);
  if(h->cls == LARGE) {
    LargeHdr *l = (LargeHdr *)ptr - 1;
    Remove((struct Node *)&l->node);
    FreeVec(l);
  } else {
    FreeBlock *f = (FreeBlock *)h;
    f->next = freeList[h->cls];
    freeList[h->cls] = f;
  }
}

void *wpdfMemRealloc(void *old, size_t size)
{
  BlockHdr *h;
  void *p;

  if(old == NULL) {
    return wpdfMemAlloc(size);
  }
  if(size == 0) {
    wpdfMemFree(old);
    return NULL;
  }
  h = (BlockHdr *)old - 1;
  /* Still fits the block it has: MuPDF grows its arrays a little at a time. */
  if(h->cls != LARGE && size + sizeof(BlockHdr) <= h->cls * GRANULE) {
    account((long)size - (long)h->size);
    h->size = size;
    return old;
  }
  p = wpdfMemAlloc(size);
  if(p == NULL) {
    return NULL;
  }
  memcpy(p, old, (h->size < size) ? h->size : size);
  wpdfMemFree(old);
  return p;
}

void wpdfMemCleanup(void)
{
  struct MinNode *n;

  while(chunks != NULL) {
    UBYTE *next = *(UBYTE **)chunks;
    FreeVec(chunks);
    chunks = next;
  }
  if(largeInit) {
    while((n = (struct MinNode *)RemHead((struct List *)&large)) != NULL) {
      FreeVec(n);
    }
  }
  memset(freeList, 0, sizeof(freeList));
  carve = carveEnd = NULL;
  inUse = 0;
}

size_t wpdfMemInUse(void)
{
  return inUse;
}

size_t wpdfMemPeak(void)
{
  return peak;
}

void wpdfMemResetPeak(void)
{
  peak = inUse;
}

unsigned long wpdfMemOsCalls(void)
{
  return osCalls;
}

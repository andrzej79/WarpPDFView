#ifndef WPDFMEM_H
#define WPDFMEM_H

/**
 * MuPDF's heap.
 *
 * vbcc's malloc() is one AllocPooled() per call and free() one FreePooled(),
 * and Exec's FreePooled() walks the pool's puddles to find the block's owner:
 * with a couple of MB live that list is long, and MuPDF allocates a few small
 * blocks for every path and glyph.  A chart page of 5600 strokes made ~19000
 * of each per pass, which the 68060 paid for in seconds.
 *
 * Here small blocks come from per-size free lists carved out of 64 KB chunks -
 * a malloc or free is a few instructions and no OS call - and only large ones
 * (pixmaps, stream buffers) go to AllocVec().  Freed small blocks are reused,
 * never returned before wpdfMemCleanup(), which frees everything.
 *
 * Not reentrant: MuPDF is called from one task.  A render task alongside the
 * UI will need a semaphore round it.
 */

#include <stddef.h>

void *wpdfMemAlloc(size_t size);
void *wpdfMemRealloc(void *old, size_t size);
void wpdfMemFree(void *ptr);
void wpdfMemCleanup(void);

// Bytes MuPDF currently holds and the most it has held since the last reset.
size_t wpdfMemInUse(void);
size_t wpdfMemPeak(void);
void wpdfMemResetPeak(void);
// OS calls made: chunk and large-block AllocVec()s.
unsigned long wpdfMemOsCalls(void);

#endif

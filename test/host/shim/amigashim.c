/* Host shim for Exec, DOS and timer.device - see amigashim.h. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "amigashim.h"

static __thread struct Task *self = NULL;

// ---- memory and lists ----------------------------------------------------------------

void *AllocVec(ULONG size, ULONG flags)
{
  return (flags & MEMF_CLEAR) ? calloc(1, size) : malloc(size);
}

void FreeVec(void *p)
{
  free(p);
}

void NewList(struct List *l)
{
  l->lh_Head = (struct Node *)&l->lh_Tail;
  l->lh_Tail = NULL;
  l->lh_TailPred = (struct Node *)&l->lh_Head;
}

void AddTail(struct List *l, struct Node *n)
{
  struct Node *pred = l->lh_TailPred;

  n->ln_Succ = (struct Node *)&l->lh_Tail;
  n->ln_Pred = pred;
  pred->ln_Succ = n;
  l->lh_TailPred = n;
}

void Remove(struct Node *n)
{
  n->ln_Pred->ln_Succ = n->ln_Succ;
  n->ln_Succ->ln_Pred = n->ln_Pred;
}

struct Node *RemHead(struct List *l)
{
  struct Node *n = l->lh_Head;

  if(n->ln_Succ == NULL) {
    return NULL;
  }
  Remove(n);
  return n;
}

// ---- semaphores ------------------------------------------------------------------------

void InitSemaphore(struct SignalSemaphore *s)
{
  pthread_mutexattr_t a;

  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&s->m, &a);
}

void ObtainSemaphore(struct SignalSemaphore *s)
{
  pthread_mutex_lock(&s->m);
}

void ReleaseSemaphore(struct SignalSemaphore *s)
{
  pthread_mutex_unlock(&s->m);
}

// ---- tasks and signals ------------------------------------------------------------------

static void initTask(struct Task *t)
{
  pthread_mutex_init(&t->m, NULL);
  pthread_cond_init(&t->c, NULL);
  t->alloc = 0xFFFF;   // the system's
  t->recvd = 0;
}

void shimInitMainTask(void)
{
  self = calloc(1, sizeof(struct Task));
  initTask(self);
  self->th = pthread_self();
}

struct Task *FindTask(const char *name)
{
  (void)name;
  return self;
}

BYTE AllocSignal(LONG n)
{
  int i;

  (void)n;
  for(i = 16; i < 32; i++) {
    if(!(self->alloc & (1UL << i))) {
      self->alloc |= 1UL << i;
      return (BYTE)i;
    }
  }
  return -1;
}

void FreeSignal(LONG n)
{
  if(n >= 0) {
    self->alloc &= ~(1UL << n);
  }
}

void Signal(struct Task *t, ULONG mask)
{
  pthread_mutex_lock(&t->m);
  t->recvd |= mask;
  pthread_cond_broadcast(&t->c);
  pthread_mutex_unlock(&t->m);
}

ULONG Wait(ULONG mask)
{
  ULONG got;

  pthread_mutex_lock(&self->m);
  while(!(self->recvd & mask)) {
    pthread_cond_wait(&self->c, &self->m);
  }
  got = self->recvd & mask;
  self->recvd &= ~got;
  pthread_mutex_unlock(&self->m);
  return got;
}

ULONG SetSignal(ULONG newSigs, ULONG mask)
{
  ULONG old;

  pthread_mutex_lock(&self->m);
  old = self->recvd;
  self->recvd = (old & ~mask) | (newSigs & mask);
  pthread_mutex_unlock(&self->m);
  return old;
}

void Forbid(void)
{
}

void Permit(void)
{
}

static void *trampoline(void *arg)
{
  struct Task *t = arg;

  self = t;
  t->entry();
  return NULL;
}

struct Process *CreateNewProcTags(ULONG tag, ...)
{
  va_list ap;
  struct Process *p = calloc(1, sizeof(struct Process));
  pthread_attr_t a;

  initTask(&p->t);
  va_start(ap, tag);
  while(tag != TAG_DONE) {
    ULONG data = va_arg(ap, ULONG);
    if(tag == NP_Entry) {
      p->t.entry = (void (*)(void))data;
    }
    tag = va_arg(ap, ULONG);
  }
  va_end(ap);
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 8 << 20);
  pthread_create(&p->t.th, &a, trampoline, &p->t);
  pthread_detach(p->t.th);
  return p;
}

// ---- timer ------------------------------------------------------------------------------

struct MsgPort *CreateMsgPort(void)
{
  return calloc(1, sizeof(struct MsgPort));
}

void DeleteMsgPort(struct MsgPort *p)
{
  free(p);
}

struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size)
{
  (void)p;
  return calloc(1, size);
}

void DeleteIORequest(struct IORequest *io)
{
  free(io);
}

static struct Device timerDevice;

LONG OpenDevice(const char *name, ULONG unit, struct IORequest *io, ULONG flags)
{
  (void)name;
  (void)unit;
  (void)flags;
  io->io_Device = &timerDevice;
  return 0;
}

void CloseDevice(struct IORequest *io)
{
  (void)io;
}

ULONG ReadEClock(struct EClockVal *ev)
{
  struct timespec ts;
  unsigned long long us;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  us = (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000ULL;
  ev->ev_hi = (ULONG)(us >> 32);
  ev->ev_lo = (ULONG)(us & 0xFFFFFFFFULL);
  return 1000000;
}

#ifndef AMIGASHIM_H
#define AMIGASHIM_H

/* Just enough of Exec, DOS and timer.device to run src/viewer/worker.c and
   src/common/wpdfmem.c on the host, over POSIX threads: tasks are threads,
   signals are per-thread bit masks with a condition variable, semaphores are
   recursive mutexes.  For testing the worker's logic - job order, aborts,
   pinning against eviction, search - not its Amiga behaviour. */

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef unsigned char UBYTE;
typedef signed char BYTE;
typedef short WORD;
typedef unsigned short UWORD;
typedef int32_t LONG;
typedef uintptr_t ULONG;     // wide enough for the pointers the tag lists carry
typedef int BOOL;
typedef void *APTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef intptr_t BPTR;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define TAG_DONE 0UL
#define TAG_USER 0x80000000UL

#define MEMF_ANY   0UL
#define MEMF_CLEAR (1UL << 16)

struct Node {
  struct Node *ln_Succ, *ln_Pred;
};
struct MinNode {
  struct MinNode *mln_Succ, *mln_Pred;
};
struct List {
  struct Node *lh_Head, *lh_Tail, *lh_TailPred;
};
struct MinList {
  struct MinNode *mlh_Head, *mlh_Tail, *mlh_TailPred;
};

struct SignalSemaphore {
  pthread_mutex_t m;
};

struct Task {
  pthread_t th;
  pthread_mutex_t m;
  pthread_cond_t c;
  ULONG alloc, recvd;
  void (*entry)(void);
};
struct Process {
  struct Task t;
};

struct MsgPort {
  int dummy;
};
struct Device {
  int dummy;
};
struct IORequest {
  struct Device *io_Device;
};
struct EClockVal {
  ULONG ev_hi, ev_lo;
};
// Not "timeval": the host has one.
struct ShimTimeVal {
  ULONG tv_secs, tv_micro;
};
struct timerequest {
  struct IORequest tr_node;
  struct ShimTimeVal tr_time;
};
#define TIMERNAME "timer.device"
#define UNIT_ECLOCK 5

#define NP_Entry     (TAG_USER + 1)
#define NP_Name      (TAG_USER + 2)
#define NP_Priority  (TAG_USER + 3)
#define NP_StackSize (TAG_USER + 4)

#define SIGBREAKF_CTRL_C (1UL << 12)

// exec
void *AllocVec(ULONG size, ULONG flags);
void FreeVec(void *p);
void NewList(struct List *l);
void AddTail(struct List *l, struct Node *n);
void Remove(struct Node *n);
struct Node *RemHead(struct List *l);
void InitSemaphore(struct SignalSemaphore *s);
void ObtainSemaphore(struct SignalSemaphore *s);
void ReleaseSemaphore(struct SignalSemaphore *s);
struct Task *FindTask(const char *name);
BYTE AllocSignal(LONG n);
void FreeSignal(LONG n);
void Signal(struct Task *t, ULONG mask);
ULONG Wait(ULONG mask);
ULONG SetSignal(ULONG newSigs, ULONG mask);
void Forbid(void);
void Permit(void);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *p);
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size);
void DeleteIORequest(struct IORequest *io);
LONG OpenDevice(const char *name, ULONG unit, struct IORequest *io, ULONG flags);
void CloseDevice(struct IORequest *io);
// dos
struct Process *CreateNewProcTags(ULONG tag, ...);
// timer
ULONG ReadEClock(struct EClockVal *ev);

// The test's own: register the calling thread as a task.
void shimInitMainTask(void);

#endif

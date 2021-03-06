#ifndef __PIRATE_MARK_LAVA_H__
#define __PIRATE_MARK_LAVA_H__

// For LAVA use only

/*
 * Keep me in sync between PANDA and LAVA repos
 */

#include "pirate_mark_lava_struct.h"

#define TARGET_I386

#if !defined(TARGET_I386) && !defined(TARGET_ARM)
#error "Define your architecture (TARGET_I386 or TARGET_ARM) with -D"
#endif

const int LABEL_BUFFER = 7;
const int LABEL_BUFFER_POS = 8;
const int QUERY_BUFFER = 9;
const int GUEST_UTIL_DONE = 10;
const int LAVA_QUERY_BUFFER = 11;

#ifdef TARGET_I386
static inline
void hypercall(void *buf, unsigned long len, long label, unsigned long off,
    PirateMarkLavaInfo *pmli, int action) {
  int eax = action;
  void *ebx = buf;
  unsigned long ecx = len;
  unsigned long edx = off;
  long edi = label;
  void *esi = pmli;

  asm __volatile__
      ("push %%eax \t\n\
        push %%ebx \t\n\
        push %%ecx \t\n\
        push %%edx \t\n\
        push %%edi \t\n\
        push %%esi \t\n\
        mov  %0, %%eax \t\n\
        mov  %1, %%ebx \t\n\
        mov  %2, %%ecx \t\n\
        mov  %3, %%edx \t\n\
        mov  %4, %%edi \t\n\
        mov  %5, %%esi \t\n\
        cpuid \t\n\
        pop  %%esi \t\n\
        pop  %%edi \t\n\
        pop  %%edx \t\n\
        pop  %%ecx \t\n\
        pop  %%ebx \t\n\
        pop  %%eax \t\n\
       "
      : /* no output registers */
      : "g" (eax), "g" (ebx), "g" (ecx), "g" (edx), "g" (edi), "g" (esi) /* input operands */
      : //"eax", "ebx", "ecx", "edx" /* clobbered registers */
      );
  return;
}

#endif // TARGET_I386

#if 0
#ifdef TARGET_ARM
inline
void hypercall(void *buf, unsigned long len, long label, unsigned long off, int action) {
    unsigned long r0 = action;
    void *r1 = buf;
    unsigned long r2 = len;
    unsigned long r3 = off;
    long r4 = label;

    asm __volatile__
      ("push {%%r0-%%r4} \t\n\
        mov %%r0, %0 \t\n\
        mov %%r1, %1 \t\n\
        mov %%r2, %2 \t\n\
        mov %%r3, %3 \t\n\
        mov %%r4, %4 \t\n\
        mcr p7, 0, r0, c0, c0, 0 \t\n\
        pop {%%r0-%%r4} \t\n"
      
      : /* no output registers */
      : "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4) /* input operands */
      : "r0", "r1", "r2", "r3", "r4" /* clobbered registers */
      );
    return;
}
#endif // TARGET_ARM
#endif

/* buf is the address of the buffer to be labeled
 * label is the label to be applied to the buffer
 * len is the length of the buffer to be labeled */
static inline
void vm_label_buffer(void *buf, int label, unsigned long len,
    PirateMarkLavaInfo *pmli) {
  hypercall(buf, len, label, 0, pmli, LABEL_BUFFER);
  return;
}

/* buf is the address of the buffer to be labeled
 * len is the length of the buffer to be labeled
 * offset is currently managed by file seeking in the utils */
static inline
void vm_label_buffer_pos(void *buf, unsigned long len, int offset,
    PirateMarkLavaInfo *pmli) {
  hypercall(buf, len, 0, offset, pmli, LABEL_BUFFER_POS);
  return;
}

/* buf is the address of the buffer to be queried
 * len is the length of the buffer to be queried
 * offset is currently managed by file seeking in the utils */
static inline
void vm_query_buffer(void *buf, unsigned long len, int offset,
    PirateMarkLavaInfo *pmli) {
  hypercall(buf, len, 0, offset, pmli, QUERY_BUFFER);
  return;
}

/* buf is the address of the buffer to be queried
 * len is the length of the buffer to be queried
 * offset is currently managed by file seeking in the utils */
static inline
void vm_lava_query_buffer(void *buf, unsigned long len, int offset,
    PirateMarkLavaInfo *pmli) {
  hypercall(buf, len, 0, offset, pmli, LAVA_QUERY_BUFFER);
  return;
}

static inline
void vm_guest_util_done(){
    printf("Guest util done\n");
    hypercall(0, 0, 0, 0, 0, GUEST_UTIL_DONE);
}

#endif

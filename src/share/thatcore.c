#define _CRT_SECURE_NO_WARNINGS
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#define THATCORE_LIB
#include "thatcore.h"

/** Debugger and logger **/

void ccxassert(bool pass, const char* expr, const char* fileline) {
  if (pass) {
    cclogd("assert pass: %s", expr);
  } else {
    ccxlogger("0[E] ", "assert fail: %s at %s", expr, fileline);
  }
}

static sint ccloglevel = 2;

sint ccxlogger(const char* tag, const void* fmt, ...) {
  sint n = 0, level = tag[0] - '0';
  const byte* p = (const byte*)fmt;
  int printed = 0;
  if (level > ccloglevel) return 0;
  for (; *p; ++p) {
    if (*p != '%') continue;
    if (*(p+1) == 0) {
      ccloge("ccxlogger invalid (1) %s", fmt);
      return 0;
    }
    p += 1;
    if (*p == '%' || *p == 's') continue;
    ccloge("ccxlogger invalid (2) %s", fmt);
    return 0;
  }
  if ((n = (sint)fprintf(stdout, tag + 1)) < 0) {
    n = 0;
  }
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    if ((printed = vfprintf(stdout, (const char*)fmt, args)) > 0) {
      n += (sint)printed;
    }
    va_end(args);
  }
  if ((printed = fprintf(stdout, CCNEWLINE)) > 0) {
    n += (sint)printed;
  }
  return n;
}

void ccsetloglevel(sint level) {
  ccloglevel = level;
}

sint ccgetloglevel() {
  return ccloglevel;
}

void ccexit() {
  exit(EXIT_FAILURE);
}

/** Memory operation **/

static uint ccmultof(uint n, uint orig) {
  /* ((orig + n - 1) / n) * n = ((orig - 1) / n + 1) * n */
  return ((n == 0 || orig == 0) ? (n | orig) : ((orig - 1) / n + 1) * n));
}

static uint ccmultofpow2(byte bits, uint orig) {
  return (orig == 0 ? (1 << bits) : ((((orig - 1) >> bits) + 1) << bits));
}

static uint llalloc(struct ccheap* self, uint bytes) {
  size_t size = (size_t)(bytes = ccmultofpow2(3, bytes));
  if (bytes > (uint)size) ccloge("llalloc too large %s", ccutos(bytes));
  if ((self->start = (byte*)malloc(size)) == 0) {
    self->beyond = 0;
    ccloge("malloc fail %s", ccutos(size));
    return 0;
  }
  self->beyond = self->start + size;
  return (uint)size;
}

struct ccheap ccalloc(uint size) {
  struct ccheap heap;
  llalloc(&heap, size);
  cczerob(heap.start, heap.beyond);
  return heap;
}

struct ccheap ccallocfrom(uint size, struct ccfrom from) {
  struct ccheap heap;
  if ((size = llalloc(&heap, size))) {
    uint copied = ccopyn(from, heap.start, size);
    if (copied < size) {
      cczero(heap.start + copied, size - copied);
    }
  }
  return heap;
}

bool ccrelloc(struct ccheap* heap, uint bytes) {
  void* buffer = 0;
  size_t size = (size_t)(bytes = ccmultofpow2(3, bytes));
  if (bytes > (uint)size) ccloge("llalloc too large %s", ccutos(bytes));
  if (heap->start + size <= heap->beyond) return true;
  if ((buffer = realloc(heap->start, size) == 0) {
    ccloge("realloc fail %s", ccutos(size));
    return false;
  }
  heap->start = (byte*)buffer;
  heap->beyond = heap->start + size;
  return true;
}

void ccfree(struct ccheap* heap) {
  if (heap->start) {
    free(heap->start);
    heap->start = 0;
    heap->beyond = 0;
  }
}

uint cczero(void* start, uint bytes) {
  if (start && bytes) {
    size_t size = (size_t)bytes;
    memset(start, 0, size);
    if (bytes > (uint)size) {
      ccloge("cczero too large %s", ccutos(bytes));
    }
    return (uint)size;
  }
  return 0;
}

uint cczerob(void* start, const void* beyond) {
  if (start < beyond) {
    return cczero(start, ((byte*)beyond) - ((byte*)start));
  }
  return 0;
}

uint cccopy(struct ccfrom from, void* to) {
  if (from.start < from.beyond) {
    byte* dest = (byte*)to;
    size_t n = from.beyond - from.start;
    if (dest + n <= from.start || dest >= from.beyond) {
      memcpy(to, from.start, n);
    } else {
      memmove(to, from.start, n);
    }
    return (uint)n;
  }
  return 0;
}

uint cccopyn(struct ccfrom from, void* to, uint n) {
  if (from.start + n <= from.beyond) {
    from.beyond = from.start + n;
  }
  return cccopy(from, to);
}

uint cccopyr(struct ccfrom from, void* to) {
  if (from.start < from.beyond) {
    byte* dest = (byte*)to;
    size_t n = from.beyond - from.start;
    if (dest + n <= from.start || dest >= from.beyond) {
      while (from.beyond-- > from.start) {
        *dest++ = *(from.beyond);
      }
    } else {
      byte* last = dest + n - 1;
      memmove(dest, from.start, n);
      while (dest < last) {
        byte ch = *dest;
        *dest++ = *last;
        *last-- = ch;
      }
    }
    return (uint)n;
  }
  return 0;
}

uint cccopyrn(struct ccfrom from, void* to, uint n) {
  if (from.start + n <= from.beyond) {
    from.beyond = from.start + n;
  }
  return cccopyr(from, to);
}

struct ccfrom ccfrom(const void* start, uint n) {
  struct ccfrom from;
  n = (n && start) ? n : 0;
  from.start = n ? (const byte*)start : 0;
  from.beyond = from.start + n;
  return from;
}

struct ccfrom ccfromb(const void* start, const void* beyond) {
  struct ccfrom from;
  if (start && start < beyond) {
    from.start = (const byte*)start;
    from.beyond = (const byte*)beyond;
  } else {
    from.start = from.beyond = 0;
  }
  return from;
}

struct ccfrom ccfromcstr(const void* s) {
  struct ccfrom from;
  uint n = s ? strlen(s) : 0;
  from.start = n ? (const byte*)s : 0;
  from.beyond = from.start + n;
  return from;
}

struct ccfrom ccfromnext(struct ccfrom from, uint n) {
  if (from.start && from.start + n <= from.beyond) {
    from.start = from.start + n;
  } else {
    from.start = from.beyond = 0;
  }
  return from;
}

struct ccdest ccdest(void* start, uint n) {
  struct ccdest dest;
  n = (n && start) ? n : 0;
  dest.start = n ? (byte*)start : 0;
  dest.beyond = dest.start + n;
  return dest;
}

struct ccdest ccdestb(void* start, void* beyond) {
  struct ccdest dest;
  if (start && start < beyond) {
    dest.start = (byte*)start;
    dest.beyond = (byte*)beyond;
  } else {
    dest.start = dest.beyond = 0;
  }
  return dest;
}

struct ccdest ccdesth(struct ccheap* heap) {
  return ccdestb(heap->start, heap->beyond);
}

/** String **/

#define LLSMASK 0x0700
#define LLNSIGN 0x0100
#define CCPSIGN 0x0200
#define CCESIGN 0x0400
#define CCZFILL 0x0800
#define CCJUSTL 0x1000
#define CCFORCE 0x2000
#define CCTOHEX 0x4000

static const byte cchexchars[] = "0123456789abcdef";

static int llutos(uint n, int minlen, void* to) {
  /* 64-bit unsigned int max value 18446744073709552046
     it is 20 characters, can be contained in struct ccstring staticly */
  byte a[CCSTRING_STATIC_CHARS+1] = {0};
  byte* dest = (byte*)to;
  unsigned int flag = (minlen & 0x7f00);
  int i = 0, end = 0;
  if (dest == 0) return 0;
  if (flag & CCTOHEX) {
    a[i++] = cchexchars[n & 0x0F];
    while ((n >>= 4)) {
      a[i++] = cchexchars[n & 0x0F];
    }
  } else {
    a[i++] = (a % 10) + '0';
    while ((n /= 10)) {
      a[i++] = (a % 10) + '0';
    }
  }
  minlen = (minelen & 0xff);
  if (minlen > CCSTRING_STATIC_CHARS) minlen = CCSTRING_STATIC_CHARS;
  if (flag & LLSMASK) {
    minlen = (flag & (CCTOHEX | CCFORCE)) ? minlen - 3 : minlen - 1;
  }
  if (flag & CCZFILL) {
    while (i < minlen) a[i++] = '0';
  }
  if (flag & (CCTOHEX | CCFORCE)) {
    a[i++] = 'x';
    a[i++] = '0';
  }
  if (flag & CCSMASK) {
    a[i++] = (flag & LLNSIGN) ? '-' : ((flag & CCPSIGN) ? '+' : ' ');
  }
  if (flag & CCJUSTL) {
    if (i < minlen && (flag & CCZFILL) == 0) {
      end = minlen;
      while (i > 0) a[--end] = a[--i];
      while (i < end) a[i++] = ' ';
      i = minlen;
    }
  } else { /* default is right justification */
    if ((flag & CCZFILL) == 0) {
      while (i < minlen) a[i++] = ' ';
    }
  }
  ccdebug(
    ccassert(i <= CCSTRING_STATIC_CHARS);
  )
  while(i > 0) *dest++ = a[--i];
  *dest = 0;
  return dest - (byte*)to;
}

static struct ccstring* llsfmt(struct ccfrom* s, int minlen, struct ccstring* (*func)(struct ccstring*, struct ccfrom), struct ccstring* self) {
  byte a[CCSTRING_STATIC_CHARS+1] = {0};
  unsigned int flag = (minlen & 0x7f00);
  int blanks = 0;
  minlen = (minlen & 0xff);
  if (minlen > CCSTRING_STATIC_CHARS) minlen = CCSTRING_STATIC_CHARS;
  if (s->start < s->beyond) {
    byte* beg = s->start;
    byte* end = a + minlen;
    if (s->beyond - s->start >= minlen) return;
    if (flag & CCJUSTL) {
      while (beg < s->beyond) *a++ = *beg++;
      while (beg < end) *a++ = ' ';
    } else {
      blanks = minlen - (s->beyond - s->start);
      beg = a + blanks;
      while (blanks--) *a++ = ' ';
      while (beg < end) *a++ = *(s->start);
    }
  } else {
    blanks = minlen;
    while (blanks--) *a++ = ' ';
  }
  s->start = a;
  s->beyond = a + minlen;
  return func(self, *s);
}

union lldouble {
  double d;
  uint u;
};

static int llrots(double n, int minlen, struct ccstring* dstr) {
  byte a[CCSTRING_STATIC_CHARS+1] = {0};
  byte* dest = (byte*)to;
  int i = 0, printed = 0;
  bool negative = false;
  uint exponent = 0;
  uint fraction = 0;
  union lldouble d;
  if (dest == 0) return 0;
  d.d = n;
  negative = (d.u & 0x8000000000000000) != 0;
  exponent = (d.u & 0x7FF0000000000000) >> 52;
  fraction = (d.u & 0x000FFFFFFFFFFFFF);
  if (exponent == 0 && fraction == 0) {
    if (negative) a[i++] = '-';
    else if (flag & LLFMTFLAG_PSIGN) a[i++] = '+';
    else if (flag & LLFMTFLAG_ESIGN) a[i++] = ' ';
    a[i++] = '0'; a[i++] = '.'; a[i++] = '0';
  } else if (exponent == 0x00000000000007FF) {
    if (fraction == 0) {
      if (negative) a[i++] = '-';
      else if (flag & LLFMTFLAG_PSIGN) a[i++] = '+';
      else if (flag & LLFMTFLAG_ESIGN) a[i++] = ' ';
      a[i++] = 'I'; a[i++] = 'n'; a[i++] = 'f'; a[i++] = 'i';
      a[i++] = 'n'; a[i++] = 'i'; a[i++] = 't'; a[i++] = 'y';
    } else {
      a[i++] = 'N'; a[i++] = 'a'; a[i++] = 'N';
    }
  } else {
    if (negative) a[i++] = '-';
    else if (flag & LLFMTFLAG_PSIGN) a[i++] = '+';
    else if (flag & LLFMTFLAG_ESIGN) a[i++] = ' ';
    /* printed does not include the additional null char */
    if ((printed = snprintf((char*)(a+i), CCSTRING_STATIC_CHARS+1-i, "%#g", n)) < 0) {
      ccloge("ccrtos snprintf fail %s", strerror(errno));
      return 0;
    }
    if ((i += printed) > CCSTRING_STATIC_CHARS) {
      i = CCSTRING_STATIC_CHARS;
    }
  }
  a[i] = 0;
  return llsfmt(ccfrom(a, i), flag, minlen, to);
}

static uint llrtos(double dval, struct ccstring* dstr) {
  byte a[CCSTRING_STATIC_CHARS+1] = {0};
  byte* p = a;
  bool negative = false;
  uint exponent = 0;
  uint fraction = 0;
  uint len = 0;
  union llfloat d;
  d.f = dval;
  negative = (d.u & 0x8000000000000000) != 0;
  exponent = (d.u & 0x7FF0000000000000) >> 52;
  fraction = (d.u & 0x000FFFFFFFFFFFFF);
  if (exponent == 0 && fraction == 0) {
    if (negative) *p++ = '-';
    *p++ = '0'; *p++ = '.'; *p++ = '0';
  } else if (exponent == 0x00000000000007FF) {
    if (fraction == 0) {
      if (negative) *p++ = '-';
      *p++ = 'I'; *p++ = 'n'; *p++ = 'f'; *p++ = 'i'; *p++ = 'n'; *p++ = 'i'; *p++ = 't'; *p++ = 'y';
    } else {
      *p++ = 'N'; *p++ = 'a'; *p++ = 'N';
    }
  } else {
    /* on success, the total number of characters written is returned.
    this count does not include the additional null-character
    automatically appended at the end of the string. */
    int n = snprintf((char*)a, 32, "%#f", dval);
    if (n > 0 && n <= CCSTRING_STATIC_CHARS) {
      p += n;
    } else {
      *p++ = '0'; *p++ = '.'; *p++ = '0';
      ccloge("ccrtos convert fail");
    }
  }
  len = p - a;
  cccopy(ccfrom(a, len+1), to);
  return len;
}

struct ccstring ccemptystr = {{0}, 0, {0}, 0};

struct ccstring ccstrfromu(uint a) {
  return ccstrfromuf(a, 0);
}

struct ccstring ccstrfromuf(uint a, int fmt) {
  struct ccstring s = ccemptystr;
  return *llstrsetlen(&s, llutos(a, fmt, &s));
}

struct ccstring ccstrfromi(sint a) {
  return ccstrfromif(a, 0);
}

struct ccstring ccstrfromif(sint a, int fmt) {
  return ccstrfromuf(a < 0 ? (-a) : a, a < 0 ? (LLNSIGN | fmt) : fmt);
}

void ccstrfree(struct ccstring* self) {
  if (self->flag == 0xFF) {
    ccfree(&self->heap);
    self->len = 0;
  }
  self->flag = 0;
}

bool ccstrequal(struct ccfrom a, struct ccfrom b) {
  if (a.beyond - a.start != b.beyond - b.start) return false;
  while (a.start < a.beyond) {
    if (*(a.start++) != *(b.start++)) return false;
  }
  return true;
}

uint ccstrlen(const struct ccstring* self) {
  return (self->flag == 0xFF ? self->len : self->flag);
}

void ccstrisempty(struct ccstring* self) {
  return ccstrlen(self) == 0;
}

uint ccstrcap(const struct ccstring* self) {
  if (self->flag == 0xFF) {
    struct ccheap* heap = &self->heap;
    return (heap->start < heap->beyond) ? (heap->beyond - heap->start - 1) : 0;
  }
  return CCSTRING_STATIC_CHARS;
}

const byte* ccstocstr(const struct ccstring* self) {
  return (self->flag == 0xFF ? self->heap.start : (const byte*)self);
}

struct ccfrom ccfroms(const struct ccstring* s) {
  return ccfrom(ccstocstr(s), ccstrlen(s));
}

byte* llstrenlarge(struct ccstring* self, uint bytes) {
  if (ccstrcap(self) < bytes) {
    uint len = 2 * ccstrlen(self); /* 1-byte for zero terminal */
    bytes = len >= (bytes+1) ? len : (bytes+1);
    if (self->flag == 0xFF) {
      return (ccrelloc(&self->heap, bytes) ? self->heap.start : 0;
    }
    if ((self->heap = ccallocfrom(bytes, ccfroms(self))).start == 0) {
      return 0;
    }
    self->len = self->flag;
    self->flag = 0xFF;
    return self->heap.start;
  }
  return (byte*)ccstocstr(self);
}

struct ccstring* llstrsetlen(struct ccstring* self, uint len) {
  (self->flag == 0xFF) ? (self->len = len) : (self->flag = (byte)len);
  return self;
}

struct ccstring* ccstrsets(struct ccstring* self, struct ccfrom s) {
  uint n = 0;
  if (from.start >= from.beyond) return self;
  n = from.beyond - from.start;
  if (!llstrenlarge(self, n) || ccstrcap(self) < n) {
    ccloge("ccstrsets too large %s", ccutos(n));
    return self;
  }
  if (self->flag == 0xFF) {
    self->len = cccopy(from, self->heap.start);
    *(self->heap.start + self->len) = 0;
  } else {
    self->flag = (byte)cccopy(from, (byte*)self);
    *(((byte*)self) + self->flag) = 0;
  }
  return self;
}

struct ccstring* ccstrsetsf(struct ccstring* self, struct ccfrom s, int fmt) {
  return llsfmt(&s, fmt, ccstrsets, self);
}

struct ccstring* ccstrsetu(struct ccstring* self, uint a) {
  return ccstrsetuf(self, a, 0);
}

struct ccstring* ccstrsetuf(struct ccstring* self, uint a, int fmt) {
  return llstrsetlen(self, llutos(a, fmt, llstrenlarge(self, CCSTRING_STATIC_CHARS)));
}

struct ccstring* ccstrseti(struct ccstring* self, sint a) {
  return ccstrsetif(self, a, 0);
}

struct ccstring* ccstrsetif(struct ccstring* self, sint a, int fmt) {
  return llstrsetlen(self, llutos(a < 0 ? (-a) : a, (a < 0 ? (fmt | LLNSIGN) : fmt), llstrenlarge(self, CCSTRING_STATIC_CHARS)));
}

struct ccstring* llstradds(struct ccstring* self, struct ccstring* s) {
  return ccstradds(self, ccfromstr(s));
}

struct ccstring* ccstradds(struct ccstring* self, struct ccfrom s) {
  uint nstr = ccstrlen(self), total = 0;
  if (from.start <= from.beyond) return self;
  total = nstr + (from.start - from.beyond);
  if (!llstrenlarge(self, total) || ccstrcap(self) < total) {
    ccloge("ccstradd too large %s", ccutos(total));
    return self;
  }
  if (self->flag == 0xFF) {
    self->len = nstr + cccopy(from, self->heap.start + nstr);
    *(self->heap.start + self->len) = 0;
  } else {
    self->flag = (byte)(nstr + cccopy(from, ((byte*)self) + nstr));
    *(((byte*)self) + self->falg) = 0;
  }
  return self;
}

struct ccstring* ccstraddsf(struct ccstring* self, struct ccfrom s, int fmt) {
  return llsfmt(&s, fmt, ccstradds, self);
}

struct ccstring* ccstraddu(struct ccstring* self, uint a) {
  return ccstradduf(self, a, 0);
}

struct ccstring* ccstradduf(struct ccstring* self, uint a, int fmt) {
  return llstradds(self, &ccstrfromuf(a, fmt));
}

struct ccstring* ccstraddi(struct ccstring* self, sint a) {
  return ccstraddif(self, a, 0);
}

struct ccstring* ccstraddif(struct ccstring* self, sint a, int fmt) {
  return llstradds(self, &ccstrfromif(a, fmt);
}

/** List and queue **/

void cclistinit(struct cclistnode* self) {
  self->next = self;
  self->prev = self;
}

bool cclistvoid(struct cclistnode* self) {
  if (self->next == self) {
    ccassert(self->prev == self);
    return true;
  }
  return false;
}

void cclistadd(struct cclistnode* self, struct cclistnode* newnode) {
  newnode->next = self->next;
  self->next = newnode;
  newnode->prev = self;
  newnode->next->prev = newnode;
}

struct cclistnode* cclistrem(struct cclistnode* node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  return node;
}

void ccslistinit(struct ccsmplnode* self) {
  node->next = self;
}

bool ccslistvoid(struct ccsmplnode* self) {
  return node->next == self;
}

void ccslistadd(struct ccsmplnode* self, struct ccsmplnode* newnode) {
  newnode->next = self->next;
  self->next = newnode;
}

struct ccsmplnode* ccslistrmn(struct ccsmplnode* self) {
  struct ccsmplnode* p = self->next;
  self->next = p->next;
  return p;
}

void ccsqueueinit(struct ccsqueue* self) {
  ccslistinit(&self->list);
  self->tail = &self->list;
}

void ccsqueueadd(struct ccsqueue* self, struct ccsmplnode* newnode) {
  ccslistadd(self->tail, newnode);
  self->tail = newnode;
}

bool ccsqueuevoid(struct ccsqueue* self) {
  return (self->list.next == &self->list);
}

struct ccsmplnode* ccsqueuerem(struct ccsqueue* self) {
  struct ccsmplnode* node = 0;
  if (ccsqueuevoid(self)) {
    return 0;
  }
  node = ccslistrmn(&self->list);
  if (node == self->tail) {
    self->head.tail = &self->head;
  }
  return node;
}

void ccdqueueinit(struct ccdqueue* self) {
  cclistinit(&self->head);
}

void ccdqueueadd(struct ccdqueue* self, struct cclistnode* newnode) {
  cclistadd(&self->head, newnode);
}

bool ccdqueuevoid(struct ccdqueue* self) {
  return cclistvoid(&self->head);
}

struct cclistnode* ccdqueuerem(struct ccsqueue* self) {
  if (ccdqueuevoid(self)) return 0;
  return cclistrem(self->head.prev);
}

/** IO notification facility **/

struct ccionfslot {
  umedit pendsize;
  umedit freesize;
  struct ccsmplnode head;
};

struct ccionftbl {
  umedit size; /* hash table need has a prime number size not near 2^n */
  struct ccsmplnode freelist;
  struct ccionfslot slot[1];
};

struct ccionfnode {
  struct ccionfnode* tnext_dont_use_;
  struct ccionfnode* qnext; /* union with size/type/flag in ccmsghead */
};

struct cctqueue {
  struct ccionfnode head;
  struct ccionfnode* tail;
};

struct ccionfmgr {
  struct cctqueue queue; /* chain all ccionfmsg fifo */
  struct ccionftbl table; /* ccionfmsg are hashed in the table for quick search */
};

struct ccionfevt;
struct ccionfmsg;

bool ccionfevtvoid(struct ccionfevt* self);
struct ccionfmsg* ccionfmsgnew(struct ccionfevt* event);
struct ccionfmsg* ccionfmsgset(struct ccionfmsg* self, struct ccionfevt* event);

void cctqueueinit(struct cctqueue* self) {
  self->head.qnext = self->tail = &self->head;
}

void cctqueueadd(struct cctqueue* self, struct ccionfnode* newnode) {
  newnode->qnext = self->tail->qnext;
  self->tail->qnext = newnode;
  self->tail = newnode;
}

bool cctqueuevoid(struct cctqueue* self) {
  return (self->head->qnext == &self->head);
}

struct ccionfnode* cctqueuerem(struct cctqueue* self) {
  struct ccionfnode* node = 0;
  if (cctqueuevoid(self)) {
    return 0;
  }
  node = self->head.qnext;
  self->head.qnext = node->qnext;
  if (self->tail == node) {
    self->tail = &self->head;
  }
  return node;
}

umedit llionftblhash(struct ccionftbl* self, umedit fd) {
  return fd % self->size; /* size should be prime number not near 2^n */
}

umedit llionftblsize(byte bits) {
  /* cchashprime(bits) return a prime number < (1 << bits) */
  return cchashprime(bits);
}

void llionftbl_add_to_free_list(struct ccionftbl* self, struct ccsmplnode* node) {
  ccslistadd(&self->freelist, node);
}

struct ccionfmsg* llionftbl_get_from_free_list(struct ccionftbl* self, struct ccionfevt* event) {
  struct ccionfmsg* p = 0;
  if (((p = struct ccionfmsg*)ccslitrmn(&self->freelist)) == 0) {
    return 0;
  }
  return llionfmsgset(p, event);
}

struct ccioftable* ccionftblnew(byte sizenumbits) {
  struct ccionftbl* table = 0;
  _int size = llionftblsize(sizenumbits);
  table = (struct ccionftbl*)ccrawalloc(sizeof(struct ccionftbl) + size * sizeof(struct ccionfslot));
  table->size = size;
  while (size > 0) {
    ccslinkinit(&(table->slot[--size].head));
  }
  ccslistinit(&table->freelist);
  return table;
}

struct ccsmplnode* ccionftbladd(struct ccionftbl* self, struct ccionfevt* newevent) {
  struct ccionfmsg* msg = 0;
  struct ccionfslot* slot = 0;
  if (llionfevtvoid(newevent)) {
    ccloge("ccionftbladd invalid fd");
    return 0;
  }
  slot = self->slot + ccionftblhash(self, (umedit)newevent->fd);
  if (slot->pendsize > 0) {
    struct ccsmplnode* head = &slot->head;
    struct ccsmplnode* cur = head;
    for (; cur->next != head; cur = cur->next) {
      msg = (struct ccionfmsg*)cur->next;
      if (llionfevtvoid(&msg->event)) {
        llionftbl_add_to_free_list(self, ccslistrmn(cur));
        if (slot->freesize > 0) {
          slot->freesize -= 1;
        } else {
          cclogw("ccionftbladd invalid freesize");
        }
        continue;
      }
      if (msg->event.fd == newevent->fd) {
        msg->event.events |= newevent->events;
        return 0;
      }
    }
  }
  if ((msg = llionftbl_get_from_free_list(self, newevent)) == 0) {
    msg = ccepollmsginit(newevent);
  }
  ccslistadd(&slot->node, (struct ccsmplnode*)msg);
  slot->pendsize += 1;
  return msg;
}

/** Thread and synchronization **/

_int ccthreadindex() {
  return ccthreadself()->index;
}
void ccsetthreaddata(void* data) {
  ccthreadself()->data = data;
}
void* ccgetthreaddata() {
  return ccthreadself()->data;
}
static int argc = 0;
static char** argv = 0;
void ccsetcmdline(int c, char** v) {
  argc = c;
  argv = v;
}
struct ccfrom ccgetcmdline(struct ccfrom name) {
  return name;
}

/** Other functions **/

bool ccisprime(umedit n) {
  sint i = 0;
  if (n == 2) return true;
  if (n == 1 || (n % 2) == 0) return false; 
  for (i = 3; i*i <= n; i += 2) {
    if ((n % i) == 0) return false;
  }
  return true;
}

umedit cchashprime(byte bits) {
  umedit i = 0, n = 0, mid = 0, m = (1 << bits);
  umedit dn = 0, dm = 0; /* distance */
  umedit nprime = 0, mprime = 0;
  if (m == 0) return 0; /* max value of bits is 31 */
  if (bits < 3) return bits + 1; /* 1(2^0) 2(2^1) 4(2^2) => 1 2 3 */
  n = (1 << (bits - 1)); /* even number */
  mid = 3 * n / 2; /* middle value between n and m, even number */
  for (i = mid - 1; i > n; i -= 2) {
    if (ccisprime(i)) {
      dn = i - n;
      nprime = i;
      break;
    }
  }
  for (i = mid + 1; i < m; i += 2) {
    if (ccisprime(i)) {
      dm = m - i;
      mprime = i;
      break;
    }
  }
  return (dn > dm ? nprime : mprime);
}

/** Core test **/

void ccthattest() {
  char a[] = "012345678";
  struct ccheap heap = {0};
  byte ainit[4] = {0};
#if defined(CCDEBUG)
  cclogd("CCDEBUG true");
#else
  cclogd("CCDEBUG false");
#endif
  ccassert(heap.start == 0);
  ccassert(heap.beyond == 0);
  ccassert(ainit[0] == 0);
  ccassert(ainit[1] == 0);
  ccassert(ainit[2] == 0);
  ccassert(ainit[3] == 0);
  /* basic types */
  ccassert(sizeof(byte) == 1);
  ccassert(sizeof(int8) == 1);
  ccassert(sizeof(sshort) == 2);
  ccassert(sizeof(ushort) == 2);
  ccassert(sizeof(smedit) == 4);
  ccassert(sizeof(umedit) == 4);
  ccassert(sizeof(sint) == 8);
  ccassert(sizeof(uint) == 8);
  ccassert(sizeof(uint) >= sizeof(size_t));
  ccassert(sizeof(float) == 4);
  ccassert(sizeof(double) == 8);
  ccassert(sizeof(freal) == 4 || sizeof(freal) == 8);
  ccassert(sizeof(struct ccheap) == 16);
  ccassert(sizeof(struct ccstring) == 32);
  ccassert(sizeof(union cctypeunit) == 8);
  /* value ranges */
  ccassert(CC_BYTE_MAX == 255);
  ccassert(CC_INT8_MAX == 127);
  ccassert(CC_INT8_MIN == -127-1);
  ccassert((byte)CC_INT8_MIN == 0x80);
  ccassert((byte)CC_INT8_MIN == 128);
  ccassert(CC_USHT_MAX == 65535);
  ccassert(CC_SSHT_MAX == 32767);
  ccassert(CC_SSHT_MIN == -32767-1);
  ccassert((ushort)CC_SSHT_MIN == 32768);
  ccassert((ushort)CC_SSHT_MIN == 0x8000);
  ccassert(CC_UMED_MAX == 4294967295);
  ccassert(CC_SMED_MAX == 2147483647);
  ccassert(CC_SMED_MIN == -2147483647-1);
  ccassert((umedit)CC_SMED_MIN == 2147483648);
  ccassert((umedit)CC_SMED_MIN == 0x80000000);
  ccassert(CC_UINT_MAX == 18446744073709551615ull);
  ccassert(CC_SINT_MAX == 9223372036854775807ull);
  ccassert(CC_SINT_MIN == -9223372036854775807-1);
  ccassert((uint)CC_SINT_MIN == 9223372036854775808ull);
  ccassert((uint)CC_SINT_MIN == 0x8000000000000000ull);
  /* memory operation */
  ccassert(ccmultof(0, 0) == 0);
  ccassert(ccmultof(0, 4) == 4);
  ccassert(ccmultof(4, 0) == 4);
  ccassert(ccmultof(4, 1) == 4);
  ccassert(ccmultof(4, 2) == 4);
  ccassert(ccmultof(4, 3) == 4);
  ccassert(ccmultof(4, 4) == 4);
  ccassert(ccmultof(4, 5) == 8);
  ccassert(ccmultofpow2(0, 0) == 0);
  ccassert(ccmultofpow2(0, 4) == 4);
  ccassert(ccmultofpow2(2, 0) == 4);
  ccassert(ccmultofpow2(2, 1) == 4);
  ccassert(ccmultofpow2(2, 2) == 4);
  ccassert(ccmultofpow2(2, 3) == 4);
  ccassert(ccmultofpow2(2, 4) == 4);
  ccassert(ccmultofpow2(2, 5) == 8);
  cccopy(ccfrom(a, a+1), a+1);
  ccassert(*(a+1) == '0'); *(a+1) = '1';
  cccopy(ccfrom(a+3, a+4), a+2);
  ccassert(*(a+2) == '3'); *(a+2) = '2';
  cccopy(ccfrom(a, a+4), a+2);
  ccassert(*(a+2) == '0');
  ccassert(*(a+3) == '1');
  ccassert(*(a+4) == '2');
  ccassert(*(a+5) == '3');
  /* other tests */
  ccassert(ccisprime(0) == false);
  ccassert(ccisprime(1) == false);
  ccassert(ccisprime(2) == true);
  ccassert(ccisprime(3) == true);
  ccassert(ccisprime(4) == false);
  ccassert(ccisprime(5) == true);
  ccassert(ccisprime(CC_UMED_MAX) == false);
}

#include <stdlib.h>
#include "l_thread.h"

extern int l_thread_mem_limit;
extern int l_thread_log_bfsz;
extern void* l_thread_log_file

typedef struct {
  l_squeue queue; /* free buffer q */
  l_int size;  /* size of the queue */
  l_int total; /* total newed elems */
  l_int ttmem; /* total memory size */
  l_int limit; /* memory size limit */
} l_freebufq;

typedef struct {
  L_COMMON_BUFHEAD
} l_bufhead;

void* l_thread_ensure_bfsize(l_thread* self, l_smplnode* buffer, l_int size) {
  l_bufhead* elem = (l_bufhead*)buffer;
  l_freebufq* q = (l_freebufq*)self->frbfq;
  l_int bufsz = elem->bsize;

  if (bufsz >= size) {
    return elem;
  }

  if (size > l_max_rdwr_size) {
    l_loge_1("size %d", ld(size));
    return 0;
  }

  while (bufsz < size) {
    if (bufsz <= l_max_rdwr_size / 2) bufsz *= 2;
    else bufsz = l_max_rdwr_size;
  }

  q->ttmem += bufsz - elem->bsize;
  elem = (l_bufhead*)l_raw_realloc(elem, elem->bsize, bufsz);
  elem->bsize = bufsz;
  return elem;
}

void* l_thread_alloc_buffer(l_thread* self, l_int sizeofbuffer) {
  l_bufhead* elem = 0;
  l_freebufq* q = (l_freebufq*)self->frbfq;

  if (sizeofbuffer < sizeof(l_bufhead) || sizeofbuffer > l_max_rdwr_size) {
    l_loge_1("size %d", ld(sizeofbuffer));
    return 0;
  }

  if ((elem = (l_bufhead*)l_squeue_pop(&q->queue))) {
    q->size -= 1;
    return l_thread_ensure_bfsize(q, &elem->node, sizeofbuffer);
  }

  q->total += 1;
  q->ttmem += sizeofbuffer;
  elem = (l_bufhead*)l_raw_malloc(sizeofbuffer);
  elem->bsize = sizeofbuffer;
  return elem;
}

void* l_thread_acquire_buffer(l_thread* self, l_int sizeofbuffer) {
  l_bufhead* elem = (l_bufhead*)l_thread_alloc_buffer(self, sizeofbuffer);
  l_freebufq* q = (l_freebufq*)self->frbfq;
  q->total -= 1;
  q->ttmem -= elem->bsize;
  return elem;
}

void l_thread_release_buffer(l_thread* self, l_smplnode* buffer) {
  l_freebufq* q = (l_freebufq*)self->frbfq;
  l_thread_free_buffer(self, buffer);
  q->total += 1;
  q->ttmem += ((l_bufhead*)buffer)->bsize;
}

#define L_THREAD_MIN_BUFFER_SIZE 1024

void l_thread_free_buffer(l_thread* self, l_smplnode* buffer) {
  l_freebufq* q = (l_freebufq*)self->frbfq;
  l_int inuse = q->total - q->size;

  l_squeue_push(&q->queue, buffer);
  q->size += 1;

  if (q->ttmem < L_THREAD_MIN_BUFFER_SIZE || q->size / 4 <= inuse) {
    return;
  }

  while (q->size / 2 > inuse) {
    if (!(buffer = l_squeue_pop(&q->queue))) {
      break;
    }

    q->total -= 1;
    q->ttmem -= ((l_bufhead*)buffer)->bsize;

    q->size -= 1;
    l_raw_free(buffer);
  }
}

static void l_freebufq_init(l_freebufq* self, l_int maxlimit) {
  *self = (l_freebufq){{{0},0}, 0};
  l_squeue_init(&self->queue);
  self->limit = maxlimit;
}

static void l_freebufq_free(l_freebufq* self) {
  l_smplnode* node = 0;
  while ((node = l_squeue_pop(&self->queue))) {
    l_raw_free(node);
  }
}

static void ll_thread_init(l_thread* self) {
  l_mutex_init(self->svmtx);
  l_mutex_init(self->mutex);
  l_condv_init(self->condv);

  l_squeue_init(self->rxmq);
  l_squeue_init(self->txmq);
  l_squeue_init(self->txms);

  self->L = l_new_luastate();
  self->weight = msgwait = 0;
  l_freebufq_init((l_freebufq*)self->frbfq, l_thread_mem_limit);
  l_logger_init(self->log, l_thread_log_bfsize, l_thread_log_file);
}

static void ll_thread_free(l_thread* self) {
  l_squeue msgq;
  l_smplnode* msg = 0;
  l_close_luastate(self->L);

  l_squeue_init(&msgq);
  l_thread_lock(self);
  l_squeue_push_queue(&msgq, self->rxmq);
  l_thread_unlock(self);

  l_squeue_push_queue(&msgq, self->txmq);
  l_squeue_push_queue(&msgq, self->txms);

  while ((msg = l_squeue_pop(&msgq))) {
    l_thread_release_buffer(self, msg);
  }

  l_logger_free(self->log);
  l_freebufq_free((l_freebufq*)self->frbfq);

  l_mutex_free(self->svmtx);
  l_mutex_free(self->mutex);
  l_condv_free(self->condv);
}

static void* ll_thread_func(void* para) {
  int n = 0;
  l_thread* self = (l_thread*)para;
  l_thrkey_set_data(&llg_thread_key, self);
  n = self->start();
  ll_thread_free(self);
  return (void*)(l_int)n;
}

int l_thread_start(l_thread* self, int (*start)()) {
  ll_thread_init(self);
  self->start = start;
  return l_raw_thread_create(&self->id, ll_thread_func, self);
}

void l_thread_exit() {
  ll_thread_free(l_thread_self());
  l_raw_thread_exit();
}

void l_process_exit() {
  exit(EXIT_FAILURE);
}

int l_thread_join(l_thread* self) {
  return l_raw_thread_join(&self->id);
}


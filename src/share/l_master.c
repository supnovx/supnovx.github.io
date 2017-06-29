#include "thatcore.h"
#include "l_thread.h"
#include "l_message.h"
#include "l_service.h"

l_thrkey L_thread_key;
l_thread_local(l_thread* L_thread_ptr);

static l_thread* L_worker_threads;
static int L_num_workers;
static l_priorq L_thread_prq;

static l_thread* L_master_thread;
static l_ionfmgr L_ionf_mgr;

static l_squeue L_msg_rxq;
static l_mutex L_msg_lock;

static l_mutex L_srvc_mtx;
static l_umedit L_svid_seed;
static l_srvctable L_srvc_table;

l_thread* l_threadpool_acquire() {
  l_thread* thread = (l_thread*)l_priorq_pop(&L_thread_prq);
  thread->weight += 1;
  l_priorq_push(&L_thread_prq, &thread->node);
  return thread;
}

void l_threadpool_release(l_thread* thread) {
  thread->weight -= 1;
  l_priorq_remove(&L_thread_prq, &thread->node);
  l_priorq_push(&L_thread_prq, &thread->node);
}

typedef struct {
  l_smplnode node;
} l_srvcslot;

typedef struct {
  l_umedit nslot;
  l_umedit nelem;
  l_srvcslot* slot;
} l_srvctable;

static void l_srvctable_init(l_srvctable* self, l_byte sizebits) {
  if (sizebits > 30) {
    self->slot = 0;
    self->nelem = 0;
    l_loge_1("slots 2^%d", ld(sizebits));
    return;
  }
  self->nslot = (1 << sizebits);
  self->slot = (l_srvcslot*)l_raw_calloc(sizeof(l_srvcslot)*self->nslot);
  self->nelem = 0;
}

static l_smplnode* llheadnode(l_srvctable* self, l_umedit svid) {
  return &(self->slot[svid & (self->nslot - 1)].node);
}

static void l_srvctable_add(l_srvctable* self, l_smplnode* elem) {
  l_smplnode* slot = 0;
  if (elem == 0) return;
  slot = llheadnode(self, ((l_service*)elem)->svid);
  elem->next = slot->next;
  slot->next = elem;
  self->nelem += 1;
  if (self->nelem > self->nslot) {
    l_logw_2("nelem %d nslot %d", ld(self->nelem), ld(self->nslot));
  }
}

static l_service* l_srvctable_find(l_srvctable* self, l_umedit svid) {
  l_smplnode* slot = 0;
  l_smplnode* srvc = 0;
  slot = llheadnode(self, svid);
  srvc = slot->next;
  while (srvc) {
    if (((l_service*)srvc)->svid == svid) return (l_service*)srvc;
    srvc = srvc->next;
  }
  return 0;
}

static l_service* l_srvctable_del(l_srvctable* self, l_umedit svid) {
  l_smplnode* elem = 0;
  l_smplnode* node = 0;
  elem = llheadnode(self, svid);
  while (elem->next) {
    node = elem->next;
    if (((l_service*)node)->svid == svid) {
      elem->next = node->next;
      self->nelem -= 1;
      return (l_service*)node;
    }
    elem = node;
  }
  return 0;
}

static void l_srvctable_foreach(l_srvctable* self, void (*cb)(l_service*)) {
  l_srvcslot* slot = self->slot;
  l_srvcslot* end = slot + self->nslot;
  l_smplnode* elem = 0;
  if (self->slot == 0) return;
  for (; slot < end; ++slot) {
    elem = slot->node.next;
    while (elem) {
      if (cb) cb((l_service*)elem);
      elem = elem->next;
    }
  }
}

static void l_srvctable_clear(l_srvctable* self, void (*elemfree)(l_service*)) {
  l_srvcslot* slot = self->slot;
  l_srvcslot* end = slot + self->nslot;
  l_smplnode* first = 0;
  if (self->slot == 0) return;
  for (; slot < end; ++slot) {
    first = slot->node.next;
    while (first) {
      if (elemfree) elemfree((l_service*)first);
      slot->node.next = first->next;
    }
  }
}

static void l_srvctable_free(l_srvctable* self, void (*elemfree)(l_service*)) {
  if (!self->slot) return;
  ll_srvctable_clear(self, elemfree);
  l_raw_free(self->slot);
  self->slot = 0;
  self->nelem = 0;
}

typedef struct {
  l_squeue queue; /* free buffer q */
  l_int size;  /* size of the queue */
  l_int frmem; /* free memory size */
  l_int limit; /* free memory limit */
} l_freebq;

typedef struct {
  L_COMMON_BUFHEAD
} l_bufhead;

void* l_thread_ensure_bfsize(l_smplnode* buffer, l_int size) {
  l_bufhead* elem = (l_bufhead*)buffer;
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

  elem = (l_bufhead*)l_raw_realloc(elem, elem->bsize, bufsz);
  elem->bsize = bufsz;
  return elem;
}

void* l_thread_alloc_buffer(l_thread* self, l_int sizeofbuffer) {
  l_bufhead* elem = 0;
  l_freebufq* q = (l_freebufq*)self->frbf;

  l_debug_assert(self == l_thread_self());

  if (sizeofbuffer < (l_int)sizeof(l_bufhead) || sizeofbuffer > l_max_rdwr_size) {
    l_loge_1("size %d", ld(sizeofbuffer));
    return 0;
  }

  if ((elem = (l_bufhead*)l_squeue_pop(&q->queue))) {
    q->size -= 1;
    q->frmem -= elem->bsize;
    return l_thread_ensure_bfsize(&elem->node, sizeofbuffer);
  }

  elem = (l_bufhead*)l_raw_malloc(sizeofbuffer);
  elem->bsize = sizeofbuffer;
  return elem;
}

void l_thread_free_buffer(l_thread* self, l_smplnode* buffer) {
  l_freebufq* q = (l_freebufq*)self->frbf;

  l_debug_assert(self == l_thread_self());

  l_squeue_push(&q->queue, buffer);
  q->size += 1;
  q->frmem += ((l_bufhead*)buffer)->bsize;

  if (q->frmem <= q->limit) {
    return;
  }

  while (q->frmem > q->limit) {
    if (!(buffer = l_squeue_pop(&q->queue))) {
      break;
    }

    q->size -= 1;
    q->frmem -= ((l_bufhead*)buffer)->bsize;
    l_raw_free(buffer);
  }
}

#define L_MESSAGE_START_SERVICE 0x01
#define L_MESSAGE_CLOSE_SERVICE 0x02
#define L_MESSAGE_CLOSE_SRVCRSP 0x03
#define L_MESSAGE_CLOSE_EVENTFD 0x04
#define L_MIN_SERVICE_ID (1024*1024)

typedef struct {
  l_mutex ma;
  l_mutex mb;
  l_condv ca;
  l_squeue qa;
  l_squeue qb;
  l_squeue qc;
  l_freebq fq;
} l_thrblock;

static int l_thread_less(void* lhs, void* rhs) {
  return ((l_thread*)lhs)->weight < ((l_thread*)rhs)->weight;
}

typedef struct {
  int workers;
  int backlog;
  int service_table_size;
  l_int log_buffer_size;
  l_int thread_frmem_max_size;
  l_rune logfile[FILENAME_MAX+1];
  l_rune* prefixend;
} l_config;

l_config* l_create_config() {
  lua_State* L = 0;
  l_config* conf = (l_config*)l_raw_calloc(sizeof(l_config));
  conf->workers = 1;
  conf->service_table_size = 10; /* 2^10 = 1024 */
  conf->log_buffer_size = 1024*8;
  conf->thread_frmem_max_size = 1024;
  l_copy_l("trace", 6, conf->logfile);
  conf->prefixend = conf->logfile + 5;
  /* TODO: read config file */
  L = l_new_luastate();
  l_close_luastate(L);

  if (workers < 1) workers = 1;

  if (conf->log_buffer_size < BUFSIZ) {
    conf->log_buffer_size = BUFSIZ;
  }
  else if (log_buffer_size + BUFSIZ >= l_max_rdwr_size) {
    log_buffer_size = l_max_rdwr_size - BUFSIZ - 1;
  }
  log_buffer_size = (((log_buffer_size - 1) / BUFSIZ) + 1) * BUFSIZ;
  return conf;
}

void l_free_config(l_config* self) {
  l_raw_free(self);
}

static void l_thread_init(l_thread* t, l_config* conf) {
  l_thrblock* b = 0;
  l_rune* suffix = 0;
  b = (l_thrblock*)t->block;
  t->svmtx = &b->ma;
  t->mutex = &b->mb;
  t->condv = &b->ca;

  l_mutex_init(self->svmtx);
  l_mutex_init(self->mutex);
  l_condv_init(self->condv);

  t->rxmq = &b->qa;
  t->txmq = &b->qb;
  t->txms = &b->qc;

  l_squeue_init(t->rxmq);
  l_squeue_init(t->txmq);
  l_squeue_init(t->txms);

  t->frbq = &b->fq;
  l_zero_l(t->frbq, sizeof(l_freebq));
  l_squeue_init(&b->fq.queue);
  b->fq.limit = conf->thread_frmem_max_size;

  t->log = l_create_string(conf->log_buffer_size);

  if (l_strt_equal(l_literal_strt("stdout"), l_strt_c(conf->logfile))) {
    t->logfile.stream = stdout;
  } else if (l_strt_equal(l_literal_strt("stderr"), l_strt_c(conf->logfile))) {
    t->logfile.stream = stderr;
  } else {
    suffix = conf->prefixend;
    *suffix++ = '.';
    suffix = l_string_format_ipart(i, suffix);
    suffix = l_copy_s(l_literal_strt(".txt"), suffix);
    *suffix = 0;
    t->logfile = l_open_append_unbuffered(conf->logfile);
  }

  t->L = l_new_luastate();
  t->weight = 0 = t->msgwait = 0;
}

static void l_thread_free(l_thread* t) {
  l_squeue msgq;
  l_smplnode* node = 0;
  l_squeue* frbq = 0;

  l_squeue_init(&msgq);

  l_thread_lock(t);
  l_squeue_push_queue(&msgq, t->rxmq);
  l_thread_unlock(t);

  l_squeue_push_queue(&msgq, t->txmq);
  l_squeue_push_queue(&msgq, t->txms);

  while ((node = l_squeue_pop(&msgq))) {
    l_raw_free(node);
  }

  if (t->L) {
    l_close_luastate(t->L);
    t->L = 0;
  }

  l_string_free(&t->log);

  if (t->logfile.stream != stdout && t->logfile.stream != stderr) {
    l_close_file(&t->logfile);
  }

  frbq = &((l_freebq*)t->frbq)->queue;
  while ((node = l_squeue_pop(frbq))) {
    l_raw_free(node);
  }

  l_mutex_free(t->svmtx);
  l_mutex_free(t->mutex);
  l_condv_free(t->condv);
}

static void* l_thread_func(void* para) {
  int n = 0;
  l_thread* self = (l_thread*)para;
  L_thread_ptr = self;
  l_thrkey_set_data(&L_thread_key, self);
  n = self->start();
  l_thread_free(self);
  return (void*)(l_int)n;
}

int l_thread_start(l_thread* self, int (*start)()) {
  self->start = start;
  return l_raw_thread_create(&self->id, ll_thread_func, self);
}

void l_thread_exit() {
  l_thread_free(l_thread_self());
  l_raw_thread_exit();
}

void l_process_exit() {
  exit(EXIT_FAILURE);
}

int l_thread_join(l_thread* self) {
  return l_raw_thread_join(&self->id);
}

static void l_master_init() {
  int i = 0;
  l_config* conf = l_create_config();
  l_thread* master = 0;
  l_thread* thread = 0;

  l_thrkey_init(&L_thread_key);
  l_priorq_init(&L_thread_prq, l_thread_less);

  L_num_workers = conf->workers;
  master = L_master_thread = (l_thread*)l_raw_calloc(sizeof(l_thread) * (workers + 1));
  L_worker_threads = master + 1;

  for (i = 0; i < workers + 1; ++i) {
    thread = master + i;
    thread->block = l_raw_malloc(sizeof(l_thrblock));
    thread->index = i;
    l_thread_init(thread, i, conf);
    l_priorq_push(&L_thread_prq, &thread->node);
  }

  l_free_config(conf);

  /* init globals */

  L_thread_ptr = master;
  l_thrkey_set_data(&L_thread_key, master);

  l_socket_startup();
  l_ionfmgr_init(&L_ionf_mgr);

  l_squeue_init(&L_msg_rxq);
  l_mutex_init(&L_msg_lock);

  l_mutex_init(&L_srvc_mtx);
  L_svid_seed = L_MIN_SERVICE_ID;
  l_srvctable_init(&L_srvc_table, service_table_size);
}

static void l_master_free() {
  l_smplnode* node = 0;
  l_thread* thread = 0;

  l_thread_free(l_thread_master());

  l_ionfmgr_free(&L_ionf_mgr);

  l_mutex_lock(&L_msg_lock);
  while ((node = l_squeue_pop(&L_msg_rxq)) {
    l_raw_free(msg);
  }
  l_mutex_unlock(&L_msg_lock);
  l_mutex_free(&L_msg_lock);

  l_mutex_free(&L_srvc_mtx);
  l_srvctable_free(&L_srvc_table, l_raw_free);

  while ((thread = (l_thread*)l_priorq_pop(&L_thread_prq))) {
    l_raw_free(thread->block);
  }
  l_raw_free(L_master_thread);
  L_master_thread = 0;
  l_thrkey_free(&L_thread_key);
}

l_ionfmgr* l_master_get_ionfmgr() {
  return &L_ionf_mgr;
}

l_thread* l_master_thread() {
  return L_master_thread;
}

l_umedit l_master_gen_service_id() {
  l_mutex* mtx = &L_srvc_mtx;
  l_umedit svid = 0;

  l_mutex_lock(mtx);
  svid = ++L_svid_seed;
  if (svid < L_MIN_SERVICE_ID) {
    svid = L_svid_seed = L_MIN_SERVICE_ID;
  }
  l_mutex_unlock(mtx);

  return svid;
}

static void l_master_add_service(l_service* srvc) {
  l_srvctable_add(&L_srvc_table, &srvc->node);
}

static l_service* l_master_find_service(l_umedit svid) {
  return l_srvctable_find(&L_srvc_table, svid);
}

static l_service* l_master_del_service(l_umedit svid) {
  return l_srvctable_del(&L_srvc_table, svid);
}

static void l_master_get_messages(l_squeue* outq) {
  l_squeue* mq = &L_msg_rxq;
  l_mutex* mtx = &L_msg_lock;

  l_mutex_lock(mtx);
  l_squeue_push_queue(outq, mq);
  l_mutex_lock(mtx);
}

static void l_master_wakeup() {
  /* wakeup master to handle the message */
  l_ionfmgr_wakeup(l_master_get_ionfmgr());
}

static void l_master_send_connrsp_message(l_umedit dstid, l_event* ev) {
  l_thread* master = l_master_thread();
  l_message* msg = l_create_message(master, L_MESSAGE_CONNRSP, sizeof(l_ioevent_message));
  l_connrsp_message* p = (l_connrsp_message*)msg;
  p->sock = ev->fd;
  p->masks = ev->masks;
  l_send_message(master, dstid, msg);
}

static void l_master_accept_connection(void* ud, l_sockconn* conn) {
  l_service* sv = (l_service*)ud;
  l_thread* master = l_master_thread();
  l_message* msg = l_create_message(master, L_MESSAGE_CONNIND, sizeof(l_connind_message));
  l_connind_message* p = (l_connind_message*)msg;
  p->sock = conn->sock;
  p->remote = conn->remote;
  l_send_message(master, sv->svid, msg);
}

static void l_master_dispatch_event(l_ioevent* rxev) {
  l_thread* master = l_master_thread();
  l_service* srvc = 0;
  l_thread* thread = 0;
  l_mutex* svmtx = 0;
  l_ioevent* srev = 0;

  if (rxev->masks == 0) return;
  if (!(srvc = l_master_find_service(rxev->svid))) return;
  if (!(thread = srvc->belong)) return;
  if (!(srvc->mflgs & L_SERVICE_IO_EVENT)) return;

  svmtx = thread->svmtx;

  l_mutex_lock(svmtx);
  srev = srvc->ioev;
  if (!srev || srev->fd != rxev->fd) {
    l_mutex_unlock(svmtx);
    return;
  }

  if (srev->falgs & L_IOEVENT_FLAG_LISTEN) {
    l_mutex_unlock(svmtx);
    l_socket_accept(srev->fd, l_master_accept_connection, srvc);
    return;
  }

  if (srev->flags & L_IOEVENT_FLAG_CONNECT) {
    srev->flags &= (~L_EVENT_FLAG_CONNECT);
    l_mutex_unlock(svmtx);
    l_master_send_connrsp_message(srvc->dstid, srev);
    return;
  }

  if (srev->masks == 0) {
    l_handle fd = event->fd;
    l_umedit masks = event->masks = rxev->masks;
    l_mutex_unlock(svmtx);
    l_send_ioevent_message(master, srvc->svid, L_MESSAGE_IOEVENT, fd, masks);
    return;
  }

  srev->masks |= rxev->masks;
  l_mutex_unlock(svmtx);
}

static void l_master_messages_handle(l_squeue* frmq) {
  l_thread* master = l_master_thread();
  l_message* msg = 0;
  l_squeue msgq;

  l_squeue_init(&msgq);

  l_thread_lock(master);
  l_squeue_push_queue(&msgq, &master->rxmq);
  l_thread_unlock(master);

  while ((msg = (l_message*)l_squeue_pop(msgq))) {
    switch (msg->type) {
    case L_MESSAGE_START_SERVICE: {
        l_service* srvc = (l_service*)((l_message_ptr*)msg)->ptr;
        /* attach thread first */
        if (!srvc->belong) {
          srvc->belong = l_threadpool_acquire();
        }
        /* then add event */
        if (srvc->ioev) {
          l_ioevent event = *srvc->ioev;
          srvc->ioev->masks = 0; /* clear masks, prepare to receive */
          l_ionfmgr_add(l_master_get_ionfmgr(), &event);
        }
        /* add service online to table */
        l_master_add_service(srvc);
      }
      break;
    case L_MESSAGE_CLOSE_EVENTFD: {
        l_service_message* p = (l_service_message*)msg;
        l_handle fd = p->fd;
        l_service* srvc = 0;

        if ((srvc = l_master_find_service(p->svid))) {
          srvc->mflgs &= (~L_SERVICE_IO_EVENT);
        }

        l_ionfmgr_del(l_master_get_ionfmgr(), fd);
        l_socket_close(fd);
      }
      break;
    case L_MESSAGE_CLOSE_SERVICE: {
        l_service_message* p = (l_service_messgae*)msg;
        l_service* srvc = l_master_del_service(p->svid);
        l_handle fd = p->fd;

        if (fd != -1) {
          l_ionfmgr_del(l_master_get_ionfmgr(), fd);
          l_socket_close(fd);
        }

        if (!srvc) break;
        l_threadpool_release(srvc->belong); /* L_MESSAGE_CLOSE_SRVCRSP is the last msg */
        l_send_message_ptr(l_master_thread(), srvc->svid, L_MESSAGE_CLOSE_SRVCRSP, srvc);
      }
      break;
    default:
      l_loge_s("unknow message id");
      break;
    }

    l_squeue_push(&frmq, &msg->node);
  }
}

void l_master_start(int (*start)()) {
  l_squeue rxmq, frmq;
  l_message* msg = 0;
  l_service* srvc = 0;
  l_thread* thread = 0;
  l_thread** ta = &L_threads;
  l_squeue* mq = 0;
  int n = L_num_threads;
  int i = 0;

  /* TODO: start bootstrap service */
  (void)start;

  mq = (l_squeue)l_raw_calloc(sizeof(l_squeue) * n);
  for (; i < n; ++i) {
    l_squeue_init(mq + i);
  }

  l_squeue_init(&rxmq);
  l_squeue_init(&frmq);

  for (; ;) {
    l_ionfmgr_wait(l_master_get_ionfmgr(), l_master_dispatch_event);
    l_master_messages_handle(&frmq); /* handle master's messages */
    l_master_get_messages(&rxmq); /* messages belong to workers */
    l_squeue_push_queue(&rxmq, &master->txmq); /* master to workers' messages */

    while ((msg = (l_message*)l_squeue_pop(&rxmq))) {
      srvc = l_master_find_service(msg->dstid);
      if (!srvc && msg->type != L_MESSAGE_CLOSE_SRVCRSP) {
        l_squeue_push(&frmq, &msg->node);
        continue;
      }

      if (msg->type == L_MESSAGE_CLOSE_SRVCRSP) {
        srvc = (l_service*)((l_message_ptr*)msg)->ptr;
        thread = srvc->belong;
      } else {
        l_mutex* svmtx = 0;
        thread = srvc->belong;
        svmtx = thread->svmtx;
        l_mutex_lock(svmtx);
        if (srvc->stop_rx_msg) {
          l_mutex_unlock(svmtx);
          l_squeue_push(&frmq, &msg->node);
          continue;
        }
        l_mutex_unlock(svmtx);
      }

      msg->srvc = srvc;
      l_squeue_push(mq + thread->index, &msg->node);
    }

    for (i = 0; i < n; ++i) {
      if (l_squeue_is_empty(mq + i)) continue;
      thread = ta[i];

      l_thread_lock(thread);
      l_squeue_push_queue(&thread->rxmq, mq + i);
      if (thread->msgwait) {
        l_thread_unlock(thread);
        continue;
      }
      thread->msgwait = 1;
      l_thread_unlock(thread);

      l_condv_signal(&thread->condv);
    }

    l_free_message_queue(l_master_thread(), &frmq);
  }
}

static void l_worker_flush_messages(l_thread* self) {
  l_message* msg = 0;
  l_thread* master = l_master_thread();
  l_mutex* mtx = &L_msg_lock;
  l_squeue* txmq = &self->txmq;
  l_squeue* txms = &self->txms;
  int sent = false;

  if (!l_squeue_is_empty(txms)) {
    l_thread* master = l_master_thread();
    l_thread_lock(master);
    l_squeue_push_queue(&master->rxmq, txms);
    l_thread_unlock(master);
    sent = true;
  }

  if (!l_squeue_is_empty(txmq)) {
    l_mutex_lock(mtx);
    l_squeue_push_queue(&L_msg_rxq, txmq);
    l_mutex_unlock(mtx);
    sent = true;
  }

  if (sent) {
    l_master_wakeup();
  }
}

int l_worker_start() {
  l_squeue msgq, frmq;
  l_service* srvc = 0;
  l_message* msg = 0;
  l_thread* thread = 0;

  l_squeue_init(&msgq);
  l_squeue_init(&frmq);

  thread = l_thread_self();

  for (; ;) {
    l_thread_lock(thread);
    while (l_squeue_is_empty(&thread->rxmq)) {
      thread->msgwait = 0;
      l_condv_wait(&thread->condv, &thread->mutex);
    }
    l_squeue_push_queue(&msgq, &thread->rxmq);
    l_thread_unlock(thread);

    while ((msg = (l_message*)l_squeue_pop(&msgq))) {
      srvc = msg->srvc;

      switch (msg->type) {
      case L_MESSAGE_CONNIND: case L_MESSAGE_CONNRSP: case L_MESSAGE_IOEVENT: {
          l_mutex* svmtx = thread->svmtx;
          if (!(srvc->wflgs & L_SERVICE_IO_EVENT)) {
            l_squeue_push(&frmq, &msg->node);
            continue;
          }
          l_mutex_lock(svmtx);
          ((l_ioevent_message*)msg)->masks = srvc->ioev->masks;
          srvc->ioev->masks = 0;
          l_mutex_unlock(svmtx);
        }
        break;
      case L_MESSAGE_CLOSE_SRVCRSP:
        l_thread_free_buffer(thread, srvc);
        l_squeue_push(&frmq, &msg->node);
        continue;
      default:
        break;
      }

      if (srvc->wflgs & L_SERVICE_CLOSING) {
        l_squeue_push(&frmq, &msg->node);
        continue;
      }

      srvc->entry(srvc, msg);
      l_squeue_push(&frmq, &msg->node);

      if (!(srvc->flag & L_SERVICE_CLOSING)) {
        continue;
      }

      if (srvc->co) {
        ll_free_coroutine(thread, srvc->co);
        srvc->co = 0;
      }

      l_thread_lock(thread);
      srvc->stop_rx_msg = 1;
      l_thread_unlock(thread);

      l_close_event(srvc);
      l_send_service_message(thread, L_SERVICE_MASTER_ID, L_MESSAGE_CLOSE_SERVICE, srvc->svid, -1);
    }

    l_free_message_queue(thread, &frmq);
    ll_thread_flush_messages(thread);
  }

  return 0;
}

static void l_master_parse_command_line(int argc, char** argv) {
  (void)argc;
  (void)argv;
}

int startmainthread(int (*start)()) {
  return startmainthread(start, 0, 0);
}

int startmainthreadcv(int (*start)(), int argc, char** argv) {
  int i = 0;
  l_thread* master = 0;
  l_master_init();

  master = l_thread_master();
  master->id = l_raw_thread_self();

  l_master_parse_command_line(argc, argv);

  for (i = 0; i < L_num_workers; ++i) {
    l_thread_start(L_worker_threads + i, l_worker_start);
  }

  l_master_start(start);

  for (i = 0; i < L_num_workers; ++i) {
    l_thread_join(L_worker_threads + i);
  }

  l_master_free();
  return 0;
}


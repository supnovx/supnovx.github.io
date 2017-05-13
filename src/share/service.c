#include <stddef.h>
#include "service.h"
#include "luacapi.h"
#include "ionotify.h"

#define LLSERVICE_START_SVID 0x2001
#define LLSERVICE_INITSIZEBITS 10 /* 1024 */
#define CCSERVICE_DONE 0x01
#define CCSERVICE_FUNC 0x01
#define CCSERVICE_CCCO 0x02
#define CCSERVICE_LUAF 0x03

struct ccservice {
  /* shared with master */
  struct cclinknode node;
  struct ccsmplnode tlink;
  struct ccthread* belong; /* modify by master only */
  struct ccsqueue rxmq;
  ushort_int outflag;
  /* thread own use */
  ushort_int flag;
  umedit_int svid;
  void* udata;
  struct ccluaco* co;
  union {
  int (*func)(void* udata, void* msg);
  int (*cofunc)(struct ccluaco*);
  } u;
};

static void ccservice_init(struct ccservice* self) {
  cczeron(self, sizeof(struct ccservice));
  cclinknode_init(&self->node);
  ccsmplnode_init(&self->tlink);
  ccsqueue_init(&self->rxmq);
}

static umedit_int ll_service_getkey(void* service) {
  return ((struct ccservice*)service)->svid;
}

static int ll_service_tlink_offset() {
  return offsetof(struct ccservice, tlink);
}

struct ccthread {
  /* access by master only */
  struct cclinknode node;
  umedit_int weight;
  /* shared with master */
  struct ccdqueue srvcrxq;
  struct ccmutex mutex;
  struct cccondv condv;
  /* free access */
  umedit_int index;
  struct ccthrid id;
  /* thread own use */
  struct lua_State* L;
  int (*start)();
  struct ccdqueue srvcq;
  struct ccsqueue msgq;
  struct ccsqueue freeco;
  umedit_int freecosz;
  umedit_int totalco;
  struct ccstring defstr;
};

static nauty_bool ll_thread_less(void* elem, void* elem2) {
  struct ccthread* thread = (struct ccthread*)elem;
  struct ccthread* thread2 = (struct ccthread*)elem2;
  return thread->weight < thread2->weight;
}

static void ll_lock_thread(struct ccthread* self) {
  ccmutex_lock(&self->mutex);
}

static void ll_unlock_thread(struct ccthread* self) {
  ccmutex_unlock(&self->mutex);
}

static umedit_int ll_new_thread_index();
static void ll_free_msgs(struct ccsqueue* msgq);

void ccthread_init(struct ccthread* self) {
  cczeron(self, sizeof(struct ccthread));
  cclinknode_init(&self->node);
  ccdqueue_init(&self->srvcrxq);
  ccdqueue_init(&self->srvcq);
  ccsqueue_init(&self->msgq);
  ccsqueue_init(&self->freeco);
  self->freecosz = self->totalco = 0;

  ccmutex_init(&self->mutex);
  cccondv_init(&self->condv);

  self->L = cclua_newstate();
  self->defstr = ccemptystr();
  self->index = ll_new_thread_index();
}

void ccthread_free(struct ccthread* self) {
  struct ccluaco* co = 0;

  self->start = 0;
  if (self->L) {
   cclua_close(self->L);
   self->L = 0;
  }

  /* free all un-handled msgs */
  ll_free_msgs(&self->msgq);

  /* un-complete services are all in the hash table,
  it is no need to free them here */

  /* free all coroutines */
  while ((co = (struct ccluaco*)ccsqueue_pop(&self->freeco))) {
    ccluaco_free(co);
    ccrawfree(co);
  }

  ccmutex_free(&self->mutex);
  cccondv_free(&self->condv);
  ccstring_free(&self->defstr);
}


/**
 * threads container
 */

struct llthreads {
  struct ccpriorq priq;
  umedit_int seed;
};

static struct llthreads* ll_ccg_threads();

static void llthreads_init(struct llthreads* self) {
  ccpriorq_init(&self->priq, ll_thread_less);
  self->seed = 0;
}

static void llthreads_free(struct llthreads* self) {
  (void)self;
}

/* no protacted, can only be called by master */
static umedit_int ll_new_thread_index() {
  struct llthreads* self = ll_ccg_threads();
  return self->seed++;
}

/* no protacted, can only be called by master */
static struct ccthread* ll_get_thread_for_new_service() {
  struct llthreads* self = ll_ccg_threads();
  struct ccthread* thread = (struct ccthread*)ccpriorq_pop(&self->priq);
  thread->weight += 1;
  ccpriorq_push(&self->priq, &thread->node);
  return thread;
}

/* no protacted, can only be called by master */
static void ll_thread_finish_a_service(struct ccthread* thread) {
  struct llthreads* self = ll_ccg_threads();
  if (thread->weight > 0) {
    thread->weight -= 1;
    ccpriorq_remove(&self->priq, &thread->node);
    ccpriorq_push(&self->priq, &thread->node);
  }
}


/**
 * services container
 */

struct llservices {
  struct ccbackhash table;
  struct ccdqueue freeq;
  umedit_int freesize;
  umedit_int seed;
  struct ccmutex mutex;
};

static struct llservices* ll_ccg_services();

static void llservices_init(struct llservices* self, nauty_byte initsizebits) {
  ccbackhash_init(&self->table, initsizebits, ll_service_tlink_offset(), ll_service_getkey);
  ccdqueue_init(&self->freeq);
  ccmutex_init(&self->mutex);
  self->freesize = 0;
  self->seed = LLSERVICE_START_SVID;
}

static void ll_service_free_memory(void* service) {
  ccrawfree(service);
}

static void llservices_free(struct llservices* self) {
  struct ccservice* sv = 0;
  cchashtable_foreach(&(self->table.a), ll_service_free_memory);
  cchashtable_foreach(&(self->table.b), ll_service_free_memory);

  while ((sv = (struct ccservice*)ccdqueue_pop(&self->freeq))) {
    ll_service_free_memory(sv);
  }

  ccbackhash_free(&self->table);
  ccmutex_free(&self->mutex);
}

static umedit_int ll_new_service_id() {
  struct llservices* self = ll_ccg_services();
  umedit_int svid = 0;

  ccmutex_lock(&self->mutex);
  svid = self->seed++;
  ccmutex_unlock(&self->mutex);

  return svid;
}

static void ll_add_new_service_to_table(struct ccservice* srvc) {
  struct llservices* self = ll_ccg_services();
  ccmutex_lock(&self->mutex);
  ccbackhash_add(&self->table, srvc);
  ccmutex_unlock(&self->mutex);
}

static struct ccservice* ll_find_service(umedit_int svid) {
  struct llservices* self = ll_ccg_services();
  struct ccservice* srvc = 0;

  ccmutex_lock(&self->mutex);
  srvc = (struct ccservice*)ccbackhash_find(&self->table, svid);
  ccmutex_unlock(&self->mutex);

  return srvc;
}

static void ll_service_is_finished(struct ccservice* srvc) {
  struct llservices* self = ll_ccg_services();
  ll_thread_finish_a_service(srvc->belong);

  ccmutex_lock(&self->mutex);
  ccbackhash_del(&self->table, srvc->svid);
  ccdqueue_push(&self->freeq, &srvc->node);
  self->freesize += 1;
  ccmutex_unlock(&self->mutex);
}

/**
 * messages container
 */

struct llmessages {
  struct ccsqueue rxq;
  struct ccsqueue freeq;
  umedit_int freesize;
  struct ccmutex mutex;
};

static struct llmessages* ll_ccg_messages();
static void ll_msg_free_data(struct ccmsgnode* self);

static void llmessages_init(struct llmessages* self) {
  ccsqueue_init(&self->rxq);
  ccsqueue_init(&self->freeq);
  ccmutex_init(&self->mutex);
  self->freesize = 0;
}

static void llmessages_free(struct llmessages* self) {
  struct ccmsgnode* msg = 0;

  ccmutex_lock(&self->mutex);
  ccsqueue_pushqueue(&self->freeq, &self->rxq);
  ccmutex_unlock(&self->mutex);

  while ((msg = (struct ccmsgnode*)ccsqueue_pop(&self->freeq))) {
    ll_msg_free_data(msg);
    ccrawfree(msg);
  }

  ccmutex_free(&self->mutex);
}

static void ll_wakeup_master() {
  struct ccionfmgr* ionf = ccgetionfmgr();
  ccionfmgr_wakeup(ionf);
}

static void ll_send_message(struct ccmsgnode* msg) {
  struct llmessages* self = ll_ccg_messages();

  /* push the message */
  ccmutex_lock(&self->mutex);
  ccsqueue_push(&self->rxq, &msg->node);
  ccmutex_unlock(&self->mutex);

  /* wakeup master to handle the message */
  ll_wakeup_master();
}

static struct ccsqueue ll_get_current_rx_msgs() {
  struct llmessages* self = ll_ccg_messages();
  struct ccsqueue q;

  ccmutex_lock(&self->mutex);
  q = self->rxq;
  ccsqueue_init(&self->rxq);
  ccmutex_lock(&self->mutex);

  return q;
}

static struct ccmsgnode* ll_new_msg() {
  struct llmessages* self = ll_ccg_messages();
  struct ccmsgnode* msg = 0;

  ccmutex_lock(&self->mutex);
  msg = (struct ccmsgnode*)ccsqueue_pop(&self->freeq);
  if (self->freesize > 0) {
    self->freesize -= 1;
  }
  ccmutex_unlock(&self->mutex);

  if (msg == 0) {
    msg = (struct ccmsgnode*)ccrawalloc(sizeof(struct ccmsgnode));
  }
  msg->node.next = msg->extra = 0;
  return msg;
}

static void ll_msg_free_data(struct ccmsgnode* self) {
  self->extra = 0;
  if (self->ptr && (((unsign_ptr)self->ptr) & 0x01)) {
    ccrawfree((void*)((((unsign_ptr)self->ptr) >> 1) << 1));
  }
  self->ptr = 0;
}

static nauty_bool ll_set_msg_data(struct ccmsgnode* self, uright_int data, void* ptr, nauty_bool needfreeptr) {
  self->data = data;
  if (((unsign_ptr)self->ptr) & 0x01) {
    ccloge("pointer started at odd address");
    if (needfreeptr) {
      ccrawfree(ptr);
    }
    return false;
  }
  if (needfreeptr) {
    self->ptr = (void*)(((unsign_ptr)ptr) | 0x01);
  } else {
    self->ptr = ptr;
  }
  return true;
}

static void ll_free_msgs(struct ccsqueue* msgq) {
  struct llmessages* self = ll_ccg_messages();
  struct ccsmplnode* elem = 0, * head = 0;
  umedit_int count = 0;

  head = &msgq->head;
  for (elem = head->next; elem != head; elem = elem->next) {
    count += 1;
    ll_msg_free_data((struct ccmsgnode*)elem);
  }

  if (count) {
    ccmutex_lock(&self->mutex);
    ccsqueue_pushqueue(&self->freeq, msgq);
    self->freesize += count;
    ccmutex_unlock(&self->mutex);
  }
}

/**
 * global
 */

struct ccglobal {
  struct llthreads threads;
  struct llservices services;
  struct llmessages messages;
  struct ccionfmgr ionf;
  struct ccthrkey thrkey;
  struct ccthread* mainthread;
};

static struct ccglobal* ccG;

static struct llthreads* ll_ccg_threads() {
  return &(ccG->threads);
}

static struct llservices* ll_ccg_services() {
  return &(ccG->services);
}

static struct llmessages* ll_ccg_messages() {
  return &(ccG->messages);
}

static nauty_bool ll_set_thread_data(struct ccthread* thread) {
  return ccthrkey_setdata(&ccG->thrkey, thread);
}

static void ccglobal_init(struct ccglobal* self) {
  cczeron(self, sizeof(struct ccglobal));
  llthreads_init(&self->threads);
  llservices_init(&self->services, LLSERVICE_INITSIZEBITS);
  llmessages_init(&self->messages);

  ccionfmgr_init(&self->ionf);
  ccthrkey_init(&self->thrkey);

  ccG = self;
}

static void ccglobal_setmaster(struct ccthread* master) {
  ccG->mainthread = master;
  ll_set_thread_data(master);
}

static void ccglobal_free(struct ccglobal* self) {
  llthreads_free(&self->threads);
  llservices_free(&self->services);
  llmessages_free(&self->messages);

  ccionfmgr_free(&self->ionf);
  ccthrkey_free(&self->thrkey);
}

struct ccionfmgr* ccgetionfmgr() {
  return &(ccG->ionf);
}


/**
 * thread
 */

static struct ccluaco* ll_new_coroutine(struct ccthread* thread, struct ccservice* srvc) {
  struct ccluaco* co = 0;
  if ((co = (struct ccluaco*)ccsqueue_pop(&thread->freeco))) {
    thread->freecosz -= 1;
  } else {
    co = (struct ccluaco*)ccrawalloc(sizeof(struct ccluaco));
    thread->totalco += 1;
  }
  ccluaco_init(co, thread->L, srvc->u.cofunc, srvc);
  return co;
}

static void ll_free_coroutine(struct ccthread* thread, struct ccluaco* co) {
  ccsqueue_push(&thread->freeco, &co->node);
  thread->freecosz += 1;
}

struct ccthread* ccthread_getself() {
  return (struct ccthread*)ccthrkey_getdata(&ccG->thrkey);
}

struct ccthread* ccthread_getmaster() {
  return ccG->mainthread;
}

struct ccstring* ccthread_getdefstr() {
  return &(ccthread_getself()->defstr);
}

void ccthread_sleep(uright_int us) {
  ccplat_threadsleep(us);
}

void ccthread_exit() {
  ccthread_free(ccthread_getself());
  ccplat_threadexit();
}

int ccthread_join(struct ccthread* self) {
  return ccplat_threadjoin(&self->id);
}

static void ll_parse_command_line(int argc, char** argv) {
  (void)argc;
  (void)argv;
}

int startmainthreadcv(int (*start)(), int argc, char** argv) {
  int n = 0;
  struct ccglobal G;
  struct ccthread master;

  /* init global first */
  ccglobal_init(&G);

  /* init master thread */
  ccthread_init(&master);
  master.id = ccplat_selfthread();

  /* attach master thread */
  ccglobal_setmaster(&master);

  /* call start func */
  ll_parse_command_line(argc, argv);
  n = start();

  /* clean */
  ccthread_free(&master);
  ccglobal_free(&G);
  return n;
}

int startmainthread(int (*start)()) {
  return startmainthreadcv(start, 0, 0);
}

static void* ll_thread_func(void* para) {
  struct ccthread* self = (struct ccthread*)para;
  int n = 0;
  ll_set_thread_data(self);
  n = self->start();
  ccthread_free(self);
  return (void*)(signed_ptr)n;
}

nauty_bool ccthread_start(struct ccthread* self, int (*start)()) {
  self->start = start;
  return ccplat_createthread(&self->id, ll_thread_func, self);
}


/**
 * service
 */

static struct ccservice* ll_new_service(umedit_int svid) {
  struct llservices* self = ll_ccg_services();
  struct ccservice* srvc = 0;
  nauty_byte newcreated = false;

  ccmutex_lock(&self->mutex);
  srvc = (struct ccservice*)ccdqueue_pop(&self->freeq);
  if (self->freesize > 0) {
    self->freesize -= 1;
  }
  ccmutex_unlock(&self->mutex);

  if (srvc == 0) {
    srvc = (struct ccservice*)ccrawalloc(sizeof(struct ccservice));
    newcreated = true;
  }

  ccservice_init(srvc);
  if (svid == 0) {
    srvc->svid = ll_new_service_id();
  } else {
    srvc->svid = svid;
  }

  if (newcreated) {
    ll_add_new_service_to_table(srvc);
  }
  return srvc;
}

struct ccservice* ccservice_new() {
  return ll_new_service(0);
}

void ccservice_sendmsg(struct ccservice* self, umedit_int destsvid, uright_int data, void* ptr, nauty_bool needfreeptr) {
  struct ccmsgnode* msg = ll_new_msg();
  msg->srcid = self->svid;
  msg->dstid = destsvid;
  if (!ll_set_msg_data(msg, data, ptr, needfreeptr)) {
    return;
  }
  ll_send_message(msg);
}

void ccservice_sendmsgtomaster(struct ccservice* self, uright_int data) {
  struct ccmsgnode* msg = ll_new_msg();
  msg->extra = self;
  msg->srcid = self->svid;
  msg->dstid = 0;
  msg->data = data;
  msg->ptr = 0;
  ll_send_message(msg);
}

void ccmaster_dispatch() {
  struct ccsqueue queue;
  struct ccsqueue freeq;
  struct ccmsgnode* msg;
  struct ccservice* sv;
  struct ccthread* master = ccthread_getmaster();

  /* get all global messages */
  queue = ll_get_current_rx_msgs();

  /* prepare master's messages */
  while ((msg = (struct ccmsgnode*)ccsqueue_pop(&queue))) {
    if (msg->dstid == 0) {
      ccsqueue_push(&master->msgq, &msg->node);
    }
  }

  /* handle master's messages */
  ccsqueue_init(&freeq);
  while ((msg = (struct ccmsgnode*)ccsqueue_pop(&master->msgq))) {
    sv = (struct ccservice*)msg->extra;
    switch (msg->data) {
    case CCMSGID_SVDONE:
      /* service's received msgs (if any) */
      ccsqueue_pushqueue(&freeq, &sv->rxmq);
      /* remove service out from hash table and insert to free queue */
      ll_service_is_finished(sv);
      /* reset the service */
      sv->belong = 0;
      break;
    default:
      ccloge("unknow message id");
      break;
    }
    ccsqueue_push(&freeq, &msg->node);
  }

  /* dispatch service's messages */
  while ((msg = (struct ccmsgnode*)ccsqueue_pop(&queue))) {
    nauty_bool newattach = false;

    /* get message's dest service */
    sv = ll_find_service(msg->dstid);
    if (sv == 0) {
      ccsqueue_push(&freeq, &msg->node);
      continue;
    }

    if (sv->belong) {
      nauty_bool done = false;

      ll_lock_thread(sv->belong);
      done = (sv->outflag & CCSERVICE_DONE) != 0;
      ll_unlock_thread(sv->belong);

      if (done) {
        /* free current msg and service's received msgs (if any) */
        ccsqueue_push(&freeq, &msg->node);
        ccsqueue_pushqueue(&freeq, &sv->rxmq);

        /* when service is done, the service is already removed out from thread's service queue.
        here delete service from the hash table and insert into service's free queue */
        ll_service_is_finished(sv);

        /* reset the service */
        sv->belong = 0;
        continue;
      }
    } else {
      /* attach a thread to handle the service */
      sv->belong = ll_get_thread_for_new_service();
      newattach = true;
    }

    /* queue the service to its belong thread */
    ll_lock_thread(sv->belong);
    msg->extra = sv;
    ccsqueue_push(&sv->rxmq, &msg->node);
    if (newattach) {
      sv->outflag = sv->flag = 0;
      ccdqueue_push(&sv->belong->srvcrxq, &sv->node);
    }
    ll_unlock_thread(sv->belong);
    /* TODO: signal the thread to handle */
  }

  ll_free_msgs(&freeq);
}

int ccthread_dispatch(struct ccthread* thread) {
  struct ccdqueue doneq;
  struct ccsqueue freemq;
  struct ccservice* sv;
  struct cclinknode* head, * elem;
  struct ccmsgnode* msg;
  int n = 0, count = 0;

  ll_lock_thread(thread);
  ccdqueue_pushqueue(&thread->srvcq, &thread->srvcrxq);
  ll_unlock_thread(thread);

  if (ccdqueue_isempty(&thread->srvcq)) {
    /* no services need to run */
    return 0;
  }

  head = &(thread->srvcq.head);
  for (elem = head->next; elem != head; elem = elem->next) {
    sv = (struct ccservice*)elem;
    if (!ccsqueue_isempty(&sv->rxmq)) {
      ll_lock_thread(thread);
      ccsqueue_pushqueue(&thread->msgq, &sv->rxmq);
      ll_unlock_thread(thread);
    }
  }

  if (ccsqueue_isempty(&thread->msgq)) {
    /* services have no messages */
    return 0;
  }

  ccdqueue_init(&doneq);
  ccsqueue_init(&freemq);
  while ((msg = (struct ccmsgnode*)ccsqueue_pop(&thread->msgq))) {
    sv = (struct ccservice*)msg->extra;
    if (sv->flag & CCSERVICE_DONE) {
      ccsqueue_push(&freemq, &msg->node);
      continue;
    }

    if (sv->flag & CCSERVICE_CCCO) {
      if (sv->co == 0) {
        sv->co = ll_new_coroutine(thread, sv);
      }
      if (sv->co->L != thread->L) {
        ccloge("wrong lua state");
        ccsqueue_push(&freemq, &msg->node);
        continue;
      }
      sv->co->srvc = sv;
      sv->co->msg = msg;
      n = ccluaco_resume(sv->co);
      sv->co->srvc = sv->co->msg = 0;
    } else if (sv->flag & CCSERVICE_FUNC) {
      n = sv->u.func(sv->udata, &msg->node);
    } else if (sv->flag & CCSERVICE_LUAF) {

    }
    else {
      ccloge("invalide service type");
    }

    /* service run completed */
    if (n == 0) {
      sv->flag |= CCSERVICE_DONE;
      ccdqueue_push(&doneq, cclinknode_remove(&sv->node));
      if (sv->co) {
        ll_free_coroutine(thread, sv->co);
        sv->co = 0;
      }
    }

    /* free handled message */
    ccsqueue_push(&freemq, &msg->node);
    count += 1;
  }

  while ((sv = (struct ccservice*)ccdqueue_pop(&doneq))) {
    ll_lock_thread(thread);
    sv->outflag |= CCSERVICE_DONE;
    ll_unlock_thread(thread);

    /* currently the finished service is removed out from thread's
    service queue, but the service still pointer to this thread.
    send SVDONE message to master to reset and free this service. */
    ccservice_sendmsgtomaster(sv, CCMSGID_SVDONE);
  }

  ll_free_msgs(&freemq);
  return count;
}


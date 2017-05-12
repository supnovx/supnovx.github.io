#ifndef CCLIB_PLATIONF_H_
#define CCLIB_PLATIONF_H_
#include "ccprefix.h"

#if defined(CC_OS_LINUX)
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
/** Linux Epoll **/

#define CCEPOLL_MAX_EVENTS 64

struct llepollmgr {
  int epfd;
  int wakeupfd;
  int nready;
  int wakeupfd_added;
  int wakeup_count;
  struct ccmutex mutex;
  struct epoll_event ready[CCEPOLL_MAX_EVENTS+1];
};

#ifdef CCLIB_AUTOCONF_TOOL
#define LLIONFMGR_TYPE_BYTES sizeof(struct llepollmgr)
#define LLIONFHDL_TYPE_BYTES sizeof(int)
#define LLIONFHDL_TYPE_IS_SIGNED (1)
#endif

#elif defined(CC_OS_APPLE) || defined(CC_OS_BSD)
#include <sys/types.h>
#include <sys/event.h>
/** BSD Kqueue **/

#else
#if !defined(CC_OS_WINDOWS)
#include <poll.h>
/** Linux Poll **/

#else
/** Windows IO **/

#endif
#endif
#endif /* CCLIB_PLATIONF_H_ */

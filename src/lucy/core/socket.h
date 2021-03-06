#ifndef l_core_socket_h
#define l_core_socket_h
#include "core/base.h"
#include "core/string.h"
#include "core/fileop.h"

#define L_SOCKET_IPV4 0x01
#define L_SOCKET_IPV6 0x02

typedef struct {
  L_PLAT_IMPL_SIZE(L_SOCKADDR_SIZE);
} l_sockaddr;

typedef struct {
  l_filedesc sock;
  l_sockaddr remote;
} l_sockconn;

L_EXTERN int l_sockaddr_init(l_sockaddr* self, l_strt ip, l_ushort port);
L_EXTERN l_ushort l_sockaddr_family(l_sockaddr* self);
L_EXTERN l_ushort l_sockaddr_port(l_sockaddr* self);
L_EXTERN int l_sockaddr_ip(l_sockaddr* self, l_byte* out, l_int len);
L_EXTERN int l_sockaddr_ipstring(l_sockaddr* self, l_string* out);

L_INLINE int
l_socket_isEmpty(l_filedesc sock)
{
  return l_filedesc_isEmpty(sock);
}

L_EXTERN void l_socket_init(); /* socket global init */
L_EXTERN l_filedesc l_socket_listen(const l_sockaddr* addr, int backlog);
L_EXTERN void l_socket_accept(l_filedesc sock, void (*cb)(void*, l_sockconn*), void* ud);
L_EXTERN void l_socket_close(l_filedesc* sock);
L_EXTERN void l_socket_shutdown(l_filedesc sock, l_byte r_w_a);
L_EXTERN void l_socketconn_init(l_sockconn* self, l_strt ip, l_ushort port);
L_EXTERN int l_socket_connect(l_sockconn* conn);
L_EXTERN l_sockaddr l_socket_localaddr(l_filedesc sock);
L_EXTERN l_int l_socket_read(l_filedesc sock, void* out, l_int count, l_int* status);
L_EXTERN l_int l_socket_write(l_filedesc sock, const void* buf, l_int count, l_int* status);
L_EXTERN void l_socket_test();
L_EXTERN void l_plat_event_test();
L_EXTERN void l_plat_sock_test();

#define L_SOCKET_READ  0x01
#define L_SOCKET_WRITE 0x02
#define L_SOCKET_RDWR  0x03
#define L_SOCKET_PRI   0x04
#define L_SOCKET_RDH   0x08
#define L_SOCKET_HUP   0x10
#define L_SOCKET_ERR   0x20

#define L_SOCKET_FLAG_ADDED   0x01
#define L_SOCKET_FLAG_LISTEN  0x02
#define L_SOCKET_FLAG_CONNECT 0x04

typedef struct {
  l_filedesc fd;
  l_umedit udata;
  l_ushort masks;
  l_ushort flags;
} l_ioevent;

typedef struct {
  L_PLAT_IMPL_SIZE(L_EVENTMGR_SIZE);
} l_eventmgr;

L_EXTERN int l_eventmgr_init(l_eventmgr* self);
L_EXTERN void l_eventmgr_free(l_eventmgr* self);
L_EXTERN int l_eventmgr_add(l_eventmgr* self, l_ioevent* event);
L_EXTERN int l_eventmgr_mod(l_eventmgr* self, l_ioevent* event);
L_EXTERN int l_eventmgr_del(l_eventmgr* self, l_filedesc fd);
L_EXTERN int l_eventmgr_wait(l_eventmgr* self, void (*cb)(l_ioevent*));
L_EXTERN int l_eventmgr_tryWait(l_eventmgr* self, void (*cb)(l_ioevent*));
L_EXTERN int l_eventmgr_timedWait(l_eventmgr* self, int ms, void (*cb)(l_ioevent*));
L_EXTERN int l_eventmgr_wakeup(l_eventmgr* self);

#endif /* l_core_socket_h */


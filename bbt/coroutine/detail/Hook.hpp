#pragma once
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>

/**
 * 当协程中执行会阻塞的接口时，不能真的阻塞当前线程。
 * 
 * 因此使用我们的函数hook系统函数，实际上使用同步非阻塞。
 * 当协程调用需要阻塞的系统接口时，注册一个事件并使用poll、
 * epoll等的监听功能来轮训监听事件，当事件完成时，再让P去
 * 唤醒阻塞的协程获取资源继续执行。
 * 
 */

using g_bbt_sys_hook_socket_fn_t    = int           (*)(int /*domain*/, int /*type*/, int /*protocol*/);
using g_bbt_sys_hook_connect_fn_t   = int           (*)(int /*socket*/, const struct sockaddr* /*address*/, socklen_t /*address_len*/);
using g_bbt_sys_hook_close_fn_t     = int           (*)(int /*fd*/);
using g_bbt_sys_hook_sleep_fn_t     = unsigned int  (*)(unsigned int /*sec*/);
using g_bbt_sys_hook_read_fn_t      = ssize_t       (*)(int /*fd*/, void* /*buf*/, size_t /*nbytes*/);
using g_bbt_sys_hook_write_fn_t     = ssize_t       (*) (int /*fd*/, const void * /*buf*/, size_t /*n*/);
using g_bbt_sys_hook_accept_fn_t    = int           (*) (int /*fd*/, __SOCKADDR_ARG /*addr*/, socklen_t *__restrict /*addr_len*/);
using g_bbt_sys_hook_send_fn_t      = ssize_t       (*)(int /*fd*/, const void * /*buf*/, size_t /*len*/, int /*flags*/);
using g_bbt_sys_hook_recv_fn_t      = ssize_t       (*)(int /*fd*/, void * /*buf*/, size_t /*len*/, int /*flags*/);

static auto g_bbt_sys_hook_socket_func      = (g_bbt_sys_hook_socket_fn_t)dlsym(RTLD_NEXT, "socket");
static auto g_bbt_sys_hook_connect_func     = (g_bbt_sys_hook_connect_fn_t)dlsym(RTLD_NEXT, "connect");
static auto g_bbt_sys_hook_close_func       = (g_bbt_sys_hook_close_fn_t)dlsym(RTLD_NEXT, "close");
static auto g_bbt_sys_hook_sleep_func       = (g_bbt_sys_hook_sleep_fn_t)dlsym(RTLD_NEXT, "sleep");
static auto g_bbt_sys_hook_read_func        = (g_bbt_sys_hook_read_fn_t)dlsym(RTLD_NEXT, "read");
static auto g_bbt_sys_hook_write_func       = (g_bbt_sys_hook_write_fn_t)dlsym(RTLD_NEXT, "write");
static auto g_bbt_sys_hook_accept_func      = (g_bbt_sys_hook_accept_fn_t)dlsym(RTLD_NEXT, "accept");
static auto g_bbt_sys_hook_send_func        = (g_bbt_sys_hook_send_fn_t)dlsym(RTLD_NEXT, "send");
static auto g_bbt_sys_hook_recv_func        = (g_bbt_sys_hook_recv_fn_t)dlsym(RTLD_NEXT, "recv");

namespace bbt::coroutine
{

namespace detail
{
extern int Hook_Socket(int domain, int type, int protocol);
extern int Hook_Connect(int socket, const struct sockaddr* address, socklen_t address_len);
extern int Hook_Close(int fd);
extern int Hook_Sleep(int s);
extern int MsSleep(int ms);
extern ssize_t Hook_Read(int fd, void* buf, size_t nbytes);
extern ssize_t Hook_Write(int fd, const void* buf, size_t n);
extern int Hook_Accept(int fd, struct sockaddr* addr, socklen_t* len);
}

}
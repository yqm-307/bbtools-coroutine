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

using g_bbt_sys_hook_socket_fn_t = int(*)(int /*domain*/, int /*type*/, int /*protocol*/);
using g_bbt_sys_hook_connect_fn_t = int(*)(int /*socket*/, const struct sockaddr* /*address*/, socklen_t /*address_len*/);
using g_bbt_sys_hook_close_fn_t = int(*)(int /*fd*/);

static auto g_bbt_sys_hook_socket_func      = (g_bbt_sys_hook_socket_fn_t)dlsym(RTLD_NEXT, "socket");
static auto g_bbt_sys_hook_connect_func       = (g_bbt_sys_hook_connect_fn_t)dlsym(RTLD_NEXT, "connect");
static auto g_bbt_sys_hook_close_fun       = (g_bbt_sys_hook_close_fn_t)dlsym(RTLD_NEXT, "close");

namespace bbt::coroutine
{

namespace detail
{
extern int Hook_Socket(int domain, int type, int protocol);
extern int Hook_Connect(int socket, const struct sockaddr* address, socklen_t address_len);
extern int Hook_Close(int fd);
extern int Hook_Sleep(int ms);
}

}
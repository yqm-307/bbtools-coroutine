#pragma once
#include <sys/socket.h>
#include <sys/uio.h>
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
 * todo：更丰富的接口、支持跨平台
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
using g_bbt_sys_hook_sendto_fn_t    = ssize_t       (*)(int /*fd*/, const void * /*buf*/, size_t /*len*/, int /*flags*/, const struct sockaddr* /*dest_addr*/, socklen_t /*addrlen*/);
using g_bbt_sys_hook_recvfrom_fn_t  = ssize_t       (*)(int /*fd*/, void * /*buf*/, size_t /*len*/, int /*flags*/, struct sockaddr* /*src_addr*/, socklen_t* /*addrlen*/);
using g_bbt_sys_hook_recvmsg_fn_t   = ssize_t       (*)(int /*fd*/, struct msghdr* /*msg*/, int /*flags*/);
using g_bbt_sys_hook_sendmsg_fn_t   = ssize_t       (*)(int /*fd*/, const struct msghdr* /*msg*/, int /*flags*/);
using g_bbt_sys_hook_readv_fn_t     = ssize_t       (*)(int /*fd*/, const struct iovec* /*iov*/, int /*iovcnt*/);
using g_bbt_sys_hook_writev_fn_t    = ssize_t       (*)(int /*fd*/, const struct iovec* /*iov*/, int /*iovcnt*/);
// glibc 的 accept4 原型带 __nonnull；dlsym 指针按普通签名即可
using g_bbt_sys_hook_accept4_fn_t   = int           (*)(int /*fd*/, __SOCKADDR_ARG /*addr*/, socklen_t *__restrict /*addr_len*/, int /*flags*/);
// #229 时间系 hook：签名与 <time.h>/<unistd.h> 原型一致
using g_bbt_sys_hook_usleep_fn_t          = int     (*)(useconds_t /*usec*/);
using g_bbt_sys_hook_nanosleep_fn_t       = int     (*)(const struct timespec* /*req*/, struct timespec* /*rem*/);
using g_bbt_sys_hook_clock_nanosleep_fn_t = int     (*)(clockid_t /*clock_id*/, int /*flags*/, const struct timespec* /*req*/, struct timespec* /*rem*/);

static auto g_bbt_sys_hook_socket_func      = (g_bbt_sys_hook_socket_fn_t)dlsym(RTLD_NEXT, "socket");
static auto g_bbt_sys_hook_connect_func     = (g_bbt_sys_hook_connect_fn_t)dlsym(RTLD_NEXT, "connect");
static auto g_bbt_sys_hook_close_func       = (g_bbt_sys_hook_close_fn_t)dlsym(RTLD_NEXT, "close");
static auto g_bbt_sys_hook_sleep_func       = (g_bbt_sys_hook_sleep_fn_t)dlsym(RTLD_NEXT, "sleep");
static auto g_bbt_sys_hook_read_func        = (g_bbt_sys_hook_read_fn_t)dlsym(RTLD_NEXT, "read");
static auto g_bbt_sys_hook_write_func       = (g_bbt_sys_hook_write_fn_t)dlsym(RTLD_NEXT, "write");
static auto g_bbt_sys_hook_accept_func      = (g_bbt_sys_hook_accept_fn_t)dlsym(RTLD_NEXT, "accept");
static auto g_bbt_sys_hook_send_func        = (g_bbt_sys_hook_send_fn_t)dlsym(RTLD_NEXT, "send");
static auto g_bbt_sys_hook_recv_func        = (g_bbt_sys_hook_recv_fn_t)dlsym(RTLD_NEXT, "recv");
static auto g_bbt_sys_hook_sendto_func      = (g_bbt_sys_hook_sendto_fn_t)dlsym(RTLD_NEXT, "sendto");
static auto g_bbt_sys_hook_recvfrom_func    = (g_bbt_sys_hook_recvfrom_fn_t)dlsym(RTLD_NEXT, "recvfrom");
static auto g_bbt_sys_hook_recvmsg_func     = (g_bbt_sys_hook_recvmsg_fn_t)dlsym(RTLD_NEXT, "recvmsg");
static auto g_bbt_sys_hook_sendmsg_func     = (g_bbt_sys_hook_sendmsg_fn_t)dlsym(RTLD_NEXT, "sendmsg");
static auto g_bbt_sys_hook_readv_func       = (g_bbt_sys_hook_readv_fn_t)dlsym(RTLD_NEXT, "readv");
static auto g_bbt_sys_hook_writev_func      = (g_bbt_sys_hook_writev_fn_t)dlsym(RTLD_NEXT, "writev");
// 部分平台/静态链接场景 dlsym 可能取不到 accept4，Hook_Accept4 里做降级处理
static auto g_bbt_sys_hook_accept4_func     = (g_bbt_sys_hook_accept4_fn_t)dlsym(RTLD_NEXT, "accept4");
static auto g_bbt_sys_hook_usleep_func          = (g_bbt_sys_hook_usleep_fn_t)dlsym(RTLD_NEXT, "usleep");
static auto g_bbt_sys_hook_nanosleep_func       = (g_bbt_sys_hook_nanosleep_fn_t)dlsym(RTLD_NEXT, "nanosleep");
static auto g_bbt_sys_hook_clock_nanosleep_func = (g_bbt_sys_hook_clock_nanosleep_fn_t)dlsym(RTLD_NEXT, "clock_nanosleep");

namespace bbt::coroutine
{

namespace detail
{
extern int Hook_Socket(int domain, int type, int protocol);
extern int Hook_Connect(int socket, const struct sockaddr* address, socklen_t address_len);
extern int Hook_Close(int fd);
extern int Hook_Sleep(int ms);
extern ssize_t Hook_Read(int fd, void* buf, size_t nbytes);
extern ssize_t Hook_Write(int fd, const void* buf, size_t n);
extern int Hook_Accept(int fd, struct sockaddr* addr, socklen_t* len);
extern ssize_t Hook_Send(int fd, const void *buf, size_t len, int flags);
extern ssize_t Hook_Recv(int fd, void *buf, size_t len, int flags);
extern ssize_t Hook_SendTo(int fd, const void *buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen);
extern ssize_t Hook_RecvFrom(int fd, void *buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen);
extern ssize_t Hook_RecvMsg(int fd, struct msghdr* msg, int flags);
extern ssize_t Hook_SendMsg(int fd, const struct msghdr* msg, int flags);
extern ssize_t Hook_Readv(int fd, const struct iovec* iov, int iovcnt);
extern ssize_t Hook_Writev(int fd, const struct iovec* iov, int iovcnt);
extern int Hook_Accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);
extern int Hook_USleep(unsigned int usec);
extern int Hook_Nanosleep(const struct timespec* req, struct timespec* rem);
extern int Hook_ClockNanosleep(clockid_t clock_id, int flags, const struct timespec* req, struct timespec* rem);
}

}

// c api 接口导出声明
extern "C" {
    // 重新定义系统函数，这样所有动态库都会调用到我们的 hook 实现
    int socket(int domain, int type, int protocol);
    int connect(int socket, const struct sockaddr* address, socklen_t address_len);
    int close(int fd);
    unsigned int sleep(unsigned int sec);
    ssize_t read(int fd, void* buf, size_t nbytes);
    ssize_t write(int fd, const void* buf, size_t n);
    int accept(int fd, __SOCKADDR_ARG addr, socklen_t *__restrict addr_len);
    ssize_t send(int fd, const void *buf, size_t len, int flags);
    ssize_t recv(int fd, void *buf, size_t len, int flags);
    ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen);
    ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen);
    ssize_t recvmsg(int fd, struct msghdr *message, int flags);
    ssize_t sendmsg(int fd, const struct msghdr *message, int flags);
    ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
    ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
    int accept4(int fd, __SOCKADDR_ARG addr, socklen_t *__restrict addr_len, int flags);
    int usleep(useconds_t usec);
    int nanosleep(const struct timespec *req, struct timespec *rem);
    int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem);
}

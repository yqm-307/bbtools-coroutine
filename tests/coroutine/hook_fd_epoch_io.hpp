/**
 * @file hook_fd_epoch_io.hpp
 * @brief FD epoch 用例的 fd 调用入口：Release 走 C 符号介入，TSAN 直接走具名 Hook_*。
 *
 * Release 构建里 libbbt_coroutine 先于 libc 导出 read/write/close，测试里的 ::read
 * 因此落到 Hook_*，用例据此覆盖 C 介入面。TSAN 构建里 libtsan 先接管这些符号，
 * 调用不再到达 libbbt_coroutine：阻塞 read 会直接挂住 worker（park 不会发生），
 * close 也不推进 fd 代际——用例要验证的 park 与代际路径根本走不到。该臂改调已
 * 导出的具名入口，走同一条产品路径；两臂的断言、超时与 worker 配置一致。
 *
 * 只有必须带 hook 语义的调用改道：park 的 read、park 的 write，以及 close（产品
 * close 不分流，任何线程的 close 都要推进代际并唤醒 waiter）。喂字节/探针一类
 * 裸数据 write/recv 不 park、不登记 waiter，两臂都走原生。
 */
#pragma once
#include <sys/types.h>
#include <unistd.h>

#include <bbt/coroutine/detail/Hook.hpp>

#if defined(__SANITIZE_THREAD__) || (defined(__clang__) && __has_feature(thread_sanitizer))
#define BBT_HOOK_FD_EPOCH_DIRECT 1
#endif

#if defined(BBT_HOOK_FD_EPOCH_DIRECT)
inline ssize_t FdEpochRead(int fd, void* buf, size_t n)
{
    return bbt::coroutine::detail::Hook_Read(fd, buf, n);
}
inline ssize_t FdEpochWrite(int fd, const void* buf, size_t n)
{
    return bbt::coroutine::detail::Hook_Write(fd, buf, n);
}
inline int FdEpochClose(int fd)
{
    return bbt::coroutine::detail::Hook_Close(fd);
}
#else
inline ssize_t FdEpochRead(int fd, void* buf, size_t n)
{
    return ::read(fd, buf, n);
}
inline ssize_t FdEpochWrite(int fd, const void* buf, size_t n)
{
    return ::write(fd, buf, n);
}
inline int FdEpochClose(int fd)
{
    return ::close(fd);
}
#endif

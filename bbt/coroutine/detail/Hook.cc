#include <stdio.h>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

int Hook_Socket(int domain, int type, int protocol)
{
    return g_bbt_sys_hook_socket_func(domain, type, protocol);
}

int Hook_Connect(int socket, const struct sockaddr* address, socklen_t address_len)
{
    return g_bbt_sys_hook_connect_func(socket, address, address_len);
}

int Hook_Close(int fd)
{
    return g_bbt_sys_hook_close_func(fd);
}

int Hook_Sleep(int ms)
{
    if (ms <= 0)
        return -1;

    errno = EINVAL;

    auto current_run_co = g_bbt_coroutine_co;
    auto&poller = g_bbt_poller;
    auto event = CoPollEvent::Create(current_run_co, ms, [](Coroutine::SPtr co){
        co->OnEventTimeout();
    });

    int ret = event->RegistEvent();
    if (ret != 0)
        return ret;

    current_run_co->Yield();
    return 0;
}

}


int socket(int domain, int type, int protocol)
{
    return bbt::coroutine::detail::Hook_Socket(domain, type, protocol);
}

int connect(int socket, const struct sockaddr* address, socklen_t address_len)
{
    return bbt::coroutine::detail::Hook_Connect(socket, address, address_len);
}

int close(int fd)
{
    return bbt::coroutine::detail::Hook_Close(fd);
}

unsigned int sleep(unsigned int sec)
{
    return bbt::coroutine::detail::Hook_Sleep(sec * 1000);
}
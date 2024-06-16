#include <stdio.h>
#include <bbt/coroutine/detail/Hook.hpp>

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
    return g_bbt_sys_hook_close_fun(fd);
}

int Hook_Sleep(int ms)
{
    return -1;
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

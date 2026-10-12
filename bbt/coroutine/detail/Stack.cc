#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <bbt/core/Attribute.hpp>
#include <bbt/core/log/DebugPrint.hpp>
#include "bbt/coroutine/detail/Stack.hpp"

#ifdef BBT_COROUTINE_VALGRIND
#include <valgrind/valgrind.h>
#endif

namespace bbt::coroutine::detail
{
namespace
{
/* 实测页大小。用余数法做页对齐，不假定页大小必为二次幂。 */
size_t PageSize()
{
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        throw std::system_error(errno != 0 ? errno : EINVAL,
                                std::generic_category(), "sysconf(_SC_PAGESIZE)");
    return static_cast<size_t>(page_size);
}

/* 指针差可表示上界：任何参与 p + n 运算的大小都必须落在此范围内 */
size_t PointerDiffLimit()
{
    return static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max());
}
}

#ifdef BBT_COROUTINE_STACK_TESTING
std::atomic_bool Stack::s_test_fail_alloc{false};
std::atomic_bool Stack::s_test_fail_apply_protect{false};
std::atomic_bool Stack::s_test_fail_release_protect{false};

void Stack::_TestFailNextAlloc() noexcept
{
    s_test_fail_alloc.store(true, std::memory_order_release);
}

void Stack::_TestFailNextApplyProtect() noexcept
{
    s_test_fail_apply_protect.store(true, std::memory_order_release);
}

void Stack::_TestFailNextReleaseProtect() noexcept
{
    s_test_fail_release_protect.store(true, std::memory_order_release);
}
#endif

size_t Stack::NormalizeSize(size_t stack_size)
{
    /* 显式 0 非法：不做默认哨兵。拒绝必须发生在分配/指针运算之前。 */
    if (stack_size == 0)
        throw std::invalid_argument("coroutine stack size must be greater than zero");

    const size_t page_size = PageSize();
    const size_t remainder = stack_size % page_size;
    const size_t padding = remainder == 0 ? 0 : page_size - remainder;
    /* 先检查再加，不溢出回绕 */
    if (stack_size > std::numeric_limits<size_t>::max() - padding)
        throw std::length_error("coroutine stack size alignment overflow");

    const size_t aligned_size = stack_size + padding;
    if (aligned_size > PointerDiffLimit())
        throw std::length_error("coroutine stack size exceeds pointer difference range");
    return aligned_size;
}

Stack::Stack(size_t stack_size, bool stack_protect)
    : m_stack_protect_flag(stack_protect),
      m_useable_size(NormalizeSize(stack_size))
{
    const size_t page_size = PageSize();

    if (m_stack_protect_flag)
    {
        /* 保护页额外分配，不计入可用区 U；总量不得越过指针差范围 */
        if (m_useable_size > PointerDiffLimit() - page_size)
            throw std::length_error("coroutine stack guard page size overflow");
        m_mem_chunk_size = m_useable_size + page_size;
    }
    else
    {
        m_mem_chunk_size = m_useable_size;
    }

    m_mem_chunk = static_cast<char*>(Alloc(m_mem_chunk_size));
    if (m_mem_chunk == nullptr)
        throw std::bad_alloc();

    if (m_stack_protect_flag && _ApplyStackProtect(m_mem_chunk) != 0)
    {
        /* 不允许日志后继续创建一个声称有保护的栈：保存 errno、释放尚未保护
         * 成功的块、清空临时所有权，再抛出。 */
        const int saved_errno = errno != 0 ? errno : EINVAL;
        Free(m_mem_chunk, m_mem_chunk_size);
        m_mem_chunk = nullptr;
        m_mem_chunk_size = 0;
        m_useable_size = 0;
        m_stack_protect_flag = false;
        throw std::system_error(saved_errno, std::generic_category(),
                                "mprotect coroutine stack guard page");
    }

#ifdef BBT_COROUTINE_VALGRIND
    _RegisterValgrindStack();
#endif
}

Stack::Stack(Stack&& other)
{
    Swap(std::move(other));
}

Stack::~Stack()
{
    _Release();
}

int Stack::_ApplyStackProtect(char* mem_chunk)
{
#ifdef BBT_COROUTINE_STACK_TESTING
    if (s_test_fail_apply_protect.exchange(false, std::memory_order_acq_rel))
    {
        errno = EACCES;
        return -1;
    }
#endif
    const size_t page_size = PageSize();
    if (::mprotect(mem_chunk, page_size, PROT_NONE) < 0)
        return -1;
    return 0;
}

int Stack::_ReleaseStackProtect()
{
#ifdef BBT_COROUTINE_STACK_TESTING
    if (s_test_fail_release_protect.exchange(false, std::memory_order_acq_rel))
    {
        errno = EACCES;
        return -1;
    }
#endif
    const size_t page_size = PageSize();
    if (::mprotect(m_mem_chunk, page_size, PROT_READ | PROT_WRITE) < 0)
        return -1;
    return 0;
}

void Stack::Clear()
{
    _Release();
}

void Stack::_Release()
{
    if (m_mem_chunk == nullptr)
        return;

#ifdef BBT_COROUTINE_VALGRIND
    /* 先注销 Valgrind 栈登记，再解除保护页并归还内存：释放后不得再被当作栈跟踪 */
    _DeregisterValgrindStack();
#endif

    if (m_stack_protect_flag && _ReleaseStackProtect() != 0)
    {
        /* 解除保护失败属于不可安全 free 的系统故障：继续 free 可能仍在
         * PROT_NONE 的 malloc 块上做写操作。不从析构抛异常（析构默认
         * noexcept，抛出即 terminate），也不冒险继续 free，直接 fail-stop；
         * 仅用隔离子进程验证，不把它当成可恢复失败。 */
        bbt::core::log::WarnPrint("%s, errno : %d %s", __FUNCTION__, errno,
                                  std::strerror(errno));
        std::terminate();
    }

    /* #279：释放必须是实际执行语句，不能塞进 assert——NDEBUG 下 assert 整个被
     * 编译掉，Release 构建会泄漏每块栈内存。先释放并置空回收到空态，保证
     * Clear/析构幂等且不二次 free。
     * ret 只作保留（Free 当前恒返回 0），用本仓 BBTATTR_COMM_UNUSED 标记
     * （同 Scheduler.cc 既有用法），不吃 -Wunused-variable。 */
    BBTATTR_COMM_UNUSED const int ret = Free(m_mem_chunk, m_mem_chunk_size);
    m_mem_chunk = nullptr;
    m_mem_chunk_size = 0;
    m_useable_size = 0;
    m_stack_protect_flag = false;
    (void)ret;
}

char* Stack::StackTop() const
{
    if (m_mem_chunk == nullptr)
        return nullptr;
    return m_mem_chunk + m_mem_chunk_size;
}

char* Stack::StackBottom() const
{
    if (m_mem_chunk == nullptr || m_useable_size == 0)
        return nullptr;

    /* 有保护区时栈底是保护页之后的位置；没有保护区就是内存块起始位置 */
    return m_stack_protect_flag
        ? m_mem_chunk + PageSize()
        : m_mem_chunk;
}

char* Stack::MemChunkBegin()
{
    return m_mem_chunk;
}

size_t Stack::MemChunkSize()
{
    return m_mem_chunk_size;
}

size_t Stack::UseableSize()
{
    return m_useable_size;
}

void* Stack::Alloc(size_t len)
{
    /* mprotect 需要对齐的内存页，memalign 返回页对齐地址（start % P == 0） */
#ifdef BBT_COROUTINE_STACK_TESTING
    if (s_test_fail_alloc.exchange(false, std::memory_order_acq_rel))
        return nullptr;
#endif
    return ::memalign(PageSize(), len);
}

int Stack::Free(char* start, size_t)
{
    std::free(start);
    return 0;
}

void Stack::Swap(Stack&& other)
{
    m_mem_chunk = other.m_mem_chunk;
    m_mem_chunk_size = other.m_mem_chunk_size;
    m_stack_protect_flag = other.m_stack_protect_flag;
    m_useable_size = other.m_useable_size;

    other.m_mem_chunk = nullptr;
    other.m_mem_chunk_size = 0;
    other.m_useable_size = 0;
    other.m_stack_protect_flag = false;

#ifdef BBT_COROUTINE_VALGRIND
    /* 栈内存地址不变，登记仍然有效：把 id 转移给新对象并清空源对象，
     * 否则源对象析构时会重复注销（源 _Release 已提前返回，但 id 必须归零） */
    m_vg_stack_id = other.m_vg_stack_id;
    other.m_vg_stack_id = 0;
#endif
}

#ifdef BBT_COROUTINE_VALGRIND

void Stack::_RegisterValgrindStack()
{
    if (m_mem_chunk == nullptr || m_useable_size == 0)
        return;

    /* 只登记“可用栈”区间（栈向下生长，begin 是低地址=栈底，end 是高地址=初始SP），
     * 不含栈底保护页；保护页保持 PROT_NONE 不变 */
    const size_t protect = m_stack_protect_flag ? PageSize() : 0;
    const unsigned long begin = reinterpret_cast<unsigned long>(m_mem_chunk + protect);
    const unsigned long end = reinterpret_cast<unsigned long>(m_mem_chunk + m_mem_chunk_size);
    m_vg_stack_id = VALGRIND_STACK_REGISTER(begin, end);
}

void Stack::_DeregisterValgrindStack()
{
    if (m_vg_stack_id != 0)
    {
        VALGRIND_STACK_DEREGISTER(m_vg_stack_id);
        m_vg_stack_id = 0;
    }
}

#endif

}

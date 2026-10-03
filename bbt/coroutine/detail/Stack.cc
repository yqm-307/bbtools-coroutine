#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/types.h>

#include <bbt/core/Attribute.hpp>
#include <bbt/core/log/DebugPrint.hpp>
#include "bbt/coroutine/detail/Stack.hpp"

#ifdef BBT_COROUTINE_VALGRIND
#include <valgrind/valgrind.h>
#endif

namespace bbt::coroutine::detail
{

Stack::Stack(const size_t stack_size,const bool stack_protect)
    :m_stack_protect_flag(stack_protect)
{
    size_t pagesize = getpagesize();

    m_useable_size = stack_size % pagesize == 0 ? stack_size : (stack_size / pagesize + 1) * pagesize;

    if (m_stack_protect_flag)
    {
        // 在栈生长方向上保护栈，防止栈溢出导致的非法访问
        m_mem_chunk_size = m_useable_size + pagesize;
        m_mem_chunk = (char*)Alloc(m_mem_chunk_size);
        assert(m_mem_chunk != nullptr && "oom");    
        _ApplyStackProtect(m_mem_chunk);
    }
    else
    {
        m_mem_chunk_size = m_useable_size;
        m_mem_chunk = (char*)Alloc(m_mem_chunk_size);
        assert(m_mem_chunk != nullptr && "oom");
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
    int pagesize = getpagesize();
    void* ptail = mem_chunk;

    if (mprotect(ptail, pagesize, PROT_NONE) < 0){
        bbt::core::log::WarnPrint("%s, errno : %d %s", __FUNCTION__, errno, strerror(errno));
        return -1;
    }

    return 0;
}

int Stack::_ReleaseStackProtect()
{
    int pagesize = getpagesize();
    
    // 释放保护页的保护设置，应该对应创建时的地址
    void* protect_addr = m_mem_chunk;  // 对应 _ApplyStackProtect 中的地址
    
    if (mprotect(protect_addr, pagesize, PROT_READ | PROT_WRITE) < 0) {
        bbt::core::log::WarnPrint("%s, errno : %d %s", __FUNCTION__, errno, strerror(errno));
        return -1;
    }

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

    // 是否需要释放保护区内存
    if (m_stack_protect_flag)
        _ReleaseStackProtect();

    /* #279：释放必须是实际执行语句，不能塞进 assert——NDEBUG 下 assert 整个被
     * 编译掉，Release 构建会泄漏每块栈内存。先释放并置空回收到空态，再断言
     * 结果（保留 Debug 下的契约检查），保证 Clear/析构幂等且不二次 free。
     * ret 只被 assert 消费，NDEBUG(Release/RelWithDebInfo) 下会变成 unused variable；
     * 用本仓 BBTATTR_COMM_UNUSED 标记（同 Scheduler.cc 既有用法），不改变语义。 */
    BBTATTR_COMM_UNUSED int ret = Free(m_mem_chunk, m_mem_chunk_size);
    m_mem_chunk = nullptr;
    m_mem_chunk_size = 0;
    m_useable_size = 0;
    assert(ret == 0);
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

    if (!m_stack_protect_flag)
        return m_mem_chunk; // 没有保护区，栈底就是内存块起始位置

    // 有保护区，栈底是保护区后面的位置
    if (m_useable_size == 0)
        return nullptr; // 没有可用栈

    return m_mem_chunk + getpagesize(); // protect 区域后面开始才是可用栈
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
    static int page_size = getpagesize();
    /**
     * mprotect 需要对齐的内存页，这个接口可以获取对齐的内存页。
     * 这里是个可以优化的点，因为大块儿内存可以在库内部管理
     */
    return (char*)memalign(page_size, len);
}

int Stack::Free(char* start, size_t len)
{
    free(start);
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
    const size_t protect = m_stack_protect_flag ? getpagesize() : 0;
    unsigned long begin = reinterpret_cast<unsigned long>(m_mem_chunk + protect);
    unsigned long end   = reinterpret_cast<unsigned long>(m_mem_chunk + m_mem_chunk_size);

    m_vg_stack_id = VALGRIND_STACK_REGISTER(begin, end);
}

void Stack::_DeregisterValgrindStack()
{
    if (m_vg_stack_id != 0) {
        VALGRIND_STACK_DEREGISTER(m_vg_stack_id);
        m_vg_stack_id = 0;
    }
}

#endif

}

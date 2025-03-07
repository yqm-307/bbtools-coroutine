#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/types.h>

#include <bbt/core/log/DebugPrint.hpp>
#include "bbt/coroutine/detail/Stack.hpp"

namespace bbt::coroutine::detail
{

Stack::Stack(const size_t stack_size,const bool stack_protect)
    :m_stack_protect_flag(stack_protect)
{
    size_t pagesize = getpagesize();

    m_useable_size = stack_size;
    if (m_useable_size % pagesize == 0)
        m_useable_size = m_useable_size; 
    else
        m_useable_size = (m_useable_size / pagesize + 1) * pagesize;

    if (m_stack_protect_flag)
    {
        m_mem_chunk_size = m_useable_size + pagesize;
        m_mem_chunk = (char*)Alloc(m_mem_chunk_size);
        assert(m_mem_chunk != nullptr);    
        m_useable_stack = m_mem_chunk;   //可访问内存栈   
        _ApplyStackProtect(m_mem_chunk, m_mem_chunk_size);
    }
    else
    {
        m_mem_chunk_size = m_useable_size;
        m_mem_chunk = (char*)Alloc(m_mem_chunk_size);
        assert(m_mem_chunk != nullptr);
        m_useable_stack = m_mem_chunk;
    }
}

Stack::Stack(Stack&& other)
{
    Swap(std::move(other));
}

Stack::~Stack()
{
    _Release();
}

int Stack::_ApplyStackProtect(char* mem_chunk, size_t mem_chunk_len)
{
    int pagesize = getpagesize();
    void* ptail = mem_chunk + mem_chunk_len - pagesize;

    if (mprotect(ptail, pagesize, PROT_NONE) < 0){
        bbt::core::log::WarnPrint("%s, errno : %d %s", __FUNCTION__, errno, strerror(errno));
        return -1;
    }

    return 0;
}

int Stack::_ReleaseStackProtect()
{
    int pagesize = getpagesize();
    void* ptail = m_mem_chunk + m_mem_chunk_size - pagesize;

    if (mprotect(ptail, pagesize, PROT_READ | PROT_WRITE) < 0) {
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
    // 获取该系统内存页大小
    int pagesize = getpagesize();

    if (m_mem_chunk == nullptr)
        return;

    // 是否需要释放保护区内存
    if (m_stack_protect_flag)
        _ReleaseStackProtect();
    
    assert(Free(m_mem_chunk, m_mem_chunk_size) == 0);
}

char* Stack::MemChunkBegin()
{
    return m_useable_stack;
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

    m_useable_stack = other.m_useable_stack;
    m_useable_size = other.m_useable_size;

    other.m_mem_chunk = nullptr;
    other.m_mem_chunk_size = 0;
    other.m_useable_stack = nullptr;
    other.m_useable_size = 0;
}

}

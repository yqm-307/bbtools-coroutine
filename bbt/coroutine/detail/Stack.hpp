#pragma once
#include <memory>
#include <sys/mman.h>
#include <bbt/base/templateutil/Noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 协程栈
 * bbt的协程实现方式是固定栈大小 + 独立栈空间。
 * 栈内存结构如下，在栈底部加了内存页保护，如果读写非法页会抛出SIGSEGV信号
 * |        | <-- bottom
 * |        |  
 * |        |  
 * |        |  
 * |        | 
 * |        | <-- protect page (can`t read & write)
 */
class Stack:
    bbt::templateutil::noncopyable
{
public:
    /**
     * @brief 构造一个协程栈对象
     * 
     * @param stack_size 栈大小
     * @param memory_protect 是否启用栈保护
     */
    explicit
    Stack(const size_t stack_size,const bool stack_protect = true);
    Stack(Stack&& other);
    ~Stack();

    /**
     * @brief 获取栈内存起始指针
     */
    char*   MemChunkBegin(); 

    /**
     * @brief 内存块大小，包含 protect 块
     */
    size_t  MemChunkSize();

    /**
     * @brief 栈可用大小，不包含 protect 块
     */
    size_t  UseableSize();

    /**
     * @brief 释放资源
     */
    void    Clear();
protected:
    void*   Alloc(size_t len);
    int     Free(char* start, size_t len);

    /**
     * 栈保护，在申请的栈内存前后申请一页不可读写内存来保护。
     * 
     * 保护只是防止因为栈溢出导致的异常但是不崩溃行为，如果
     * 程序在错误的情况继续运行可能导致更严重、难以修复的行为
     */
    int     _ApplyStackProtect(char* mem_chunk, size_t mem_chunk_len);
    int     _ReleaseStackProtect();

    void    _Release();
    void    Swap(Stack&& other);
private:
    char*   m_useable_stack{nullptr};       // 可用栈，不包含 protect 内存块
    size_t  m_useable_size{0};              // 可用栈大小，不包含 protect 内存块
    char*   m_mem_chunk{nullptr};           // 内存块，包含 protect 内存块
    size_t  m_mem_chunk_size{0};            // 内存块大小，包含 protect 内存块
    bool    m_stack_protect_flag{false};    // 是否启用栈保护
};


}
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <bbt/coroutine/detail/Define.hpp>
#include <boost/noncopyable.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 协程栈
 *
 * bbt 的协程实现是固定栈大小 + 独立栈空间，栈大小在创建后不再变化
 * （不支持扩栈/换栈）。栈内存结构如下，在栈生长方向（低地址）加一页
 * 只读保护，越界读写直接 SIGSEGV：
 *
 * |            | <-- StackTop()（高地址 = 初始 SP）
 * |            |
 * |            | <-- StackBottom()（保护页之后）
 * |  protect   | <-- 一页 PROT_NONE，不属于登记的可用区
 *
 * 页对齐后的可用字节数（不含保护页）是本栈对外的唯一尺寸口径 U，
 * 也是 StackPool 分桶键的一半；另一半是 IsProtected()。同一运行时
 * 可以有多种 U 共存，不做二次幂归档、不借更大桶替代。
 */
class Stack:
    boost::noncopyable
{
public:
    /**
     * @brief 构造一个协程栈对象
     *
     * @param stack_size 请求的可用字节数，必须 > 0；内部向上页对齐
     * @param stack_protect 是否启用低地址保护页（栈自身属性）
     * @throw std::invalid_argument 请求为 0（显式 0 非法，不是默认哨兵）
     * @throw std::length_error 页取整、追加保护页或指针差可表示范围越界
     * @throw std::bad_alloc 内存申请失败
     * @throw std::system_error 建立保护页失败（携带 errno）
     */
    explicit Stack(size_t stack_size, bool stack_protect = true);
    Stack(Stack&& other);
    ~Stack();

    /**
     * @brief 页对齐规范化（Stack 与 StackPool 共用的唯一口径）
     *
     * @return 不含保护页、按实测页大小向上对齐的可用字节数 U
     */
    static size_t NormalizeSize(size_t stack_size);

    /* 保护开关是栈自身属性：归还/复用按它分桶，不再读全局配置 */
    bool IsProtected() const noexcept { return m_stack_protect_flag; }

    /**
     * @brief 获取最大栈顶指针
     */
    char* StackTop() const;

    /**
     * @brief 获取栈底指针，相当于 bp
     */
    char* StackBottom() const;

    /**
     * @brief 获取内存块起始地址（包含 protect 内存块）
     */
    char* MemChunkBegin();

    /**
     * @brief 获取内存块大小（包含 protect 内存块）
     */
    size_t MemChunkSize();

    /**
     * @brief 栈可用大小，不包含 protect 块
     */
    size_t UseableSize();

    /**
     * @brief 释放资源（幂等：归还后回空态，重复调用不二次 free）
     */
    void Clear();

#ifdef BBT_COROUTINE_STACK_TESTING
    /* 窄测试 seam：只在 BBT_COROUTINE_STACK_TESTING 构建存在。
     * 分别让「下一次」memalign / 建立保护(PROT_NONE) / 解除保护(PROT_READ|WRITE)
     * 穿过真实结果检查分支失败，一次性消费。生产构建不含声明、定义与分支，
     * 用 nm 对未 strip 的实际库做正反对照（见 design §注入真实性）。 */
    static void _TestFailNextAlloc() noexcept;            // memalign 空返回
    static void _TestFailNextApplyProtect() noexcept;     // 建立保护失败
    static void _TestFailNextReleaseProtect() noexcept;   // 解除保护失败 → fail-stop
#endif
protected:
    void* Alloc(size_t len);
    int Free(char* start, size_t len);

    /**
     * 栈保护，在申请的栈内存低地址侧留一页不可读写内存。
     *
     * 保护只是防止因为栈溢出导致的异常但是不崩溃行为，如果
     * 程序在错误的情况继续运行可能导致更严重、难以修复的行为
     */
    int _ApplyStackProtect(char* mem_chunk);
    int _ReleaseStackProtect();

    void _Release();
    void Swap(Stack&& other);
private:
    bool m_stack_protect_flag{false};    // 是否启用栈保护
    size_t m_useable_size{0};            // 可用栈大小，不包含 protect 内存块
    char* m_mem_chunk{nullptr};          // 内存块，包含 protect 内存块
    size_t m_mem_chunk_size{0};          // 内存块大小，包含 protect 内存块

#ifdef BBT_COROUTINE_VALGRIND
    /* 内存检测专用（NEED_VALGRIND=ON 才存在，默认构建不含此成员）。bbt 协程使用
     * memalign 的自定义栈，Valgrind 默认不把它当作“栈”：boost.context 每次切换
     * SP 都被 memcheck 记为 "client switching stacks?" 并放大成百万级假阳性。
     * 构造时把可用栈区间登记给 Valgrind，Clear/析构/移动时同步注销或转移，
     * 是按栈内存跟踪的根因修复；未在 Valgrind 下运行时注册返回 0，无副作用。
     * 注意：register 只消除 stack-switch 警告，不代表 definitely/indirectly lost
     * 全为假阳性——真实泄漏判定仍需独立核实，不得据此隐藏。 */
    unsigned m_vg_stack_id{0};
    void _RegisterValgrindStack();
    void _DeregisterValgrindStack();
#endif
#ifdef BBT_COROUTINE_STACK_TESTING
    static std::atomic_bool s_test_fail_alloc;
    static std::atomic_bool s_test_fail_apply_protect;
    static std::atomic_bool s_test_fail_release_protect;
#endif
};

}

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include "bbt/coroutine/detail/Stack.hpp"

BOOST_AUTO_TEST_SUITE(Coroutine_Stack)

BOOST_AUTO_TEST_CASE(t_coroutine_stack_create)
{
    auto single_stack = bbt::coroutine::detail::Stack(1024 * 4);

    std::vector<bbt::coroutine::detail::Stack> array;

    for (int i = 0; i < 1000; ++i)
    {
        array.push_back(bbt::coroutine::detail::Stack(1024*40));
    }
}

// #279：NDEBUG 下 assert(Free(...)==0) 把释放整个编译掉 → Release 构建栈内存泄漏。
// 契约：Clear() 后栈资源必须真正释放且对象回到空态（MemChunkBegin()==nullptr），
// 重复 Clear() 幂等、不二次 free。
BOOST_AUTO_TEST_CASE(t_stack_clear_releases_and_is_idempotent)
{
    bbt::coroutine::detail::Stack stk(1024 * 64, false);
    BOOST_REQUIRE(stk.MemChunkBegin() != nullptr);
    BOOST_REQUIRE(stk.MemChunkSize() > 0);

    stk.Clear();
    BOOST_CHECK(stk.MemChunkBegin() == nullptr);
    BOOST_CHECK(stk.StackTop() == nullptr);

    // 幂等：不得二次 free 同一指针（glibc 会 abort / 破坏堆）
    stk.Clear();
    BOOST_CHECK(stk.MemChunkBegin() == nullptr);
}

// RSS 探针在 stack_protect=false 时无效：memalign 的栈内存从不被写入，只是虚拟
// 预留、不驻留 RSS，free 与否都测不出增长；真要 touch 就会占满内存。故用上面
// idempotent 探针锁 bug——不真正 free 就不可能安全地把 m_mem_chunk 置 nullptr。

BOOST_AUTO_TEST_SUITE_END()
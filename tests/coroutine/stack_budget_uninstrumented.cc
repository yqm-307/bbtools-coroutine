/**
 * @file stack_budget_uninstrumented.cc
 * @brief 未插桩 helper TU：本文件固定 -fno-instrument-functions
 * （tests/CMakeLists.txt source 属性），模拟第三方库/历史对象这类
 * 插桩覆盖不到的路径。放在 unit_test 根而不放 fixture 目录——它是被测
 * 二进制的一部分，不是数据采集夹具。
 *
 * 语义：它的内部帧不会产生任何 __cyg_profile_func_enter 样本，因此
 * 诊断只能给出 PARTIAL，不能声称观测到真实峰值。
 */

namespace bbt_stack_budget_test
{

/**
 * 与插桩版 Burn 同构，但入口不打点：内部递归深度对采样不可见。
 * noinline 防止被并入调用方帧而伪装成「已观测」。
 */
__attribute__((noinline))
void UninstrumentedBurn(volatile char* sink, int depth)
{
    volatile char frame[384];
    for (unsigned i = 0; i < sizeof(frame); i += 64)
        frame[i] = static_cast<char>(i + depth);
    *sink = static_cast<char>(*sink + frame[0]);

    if (depth > 1)
        UninstrumentedBurn(sink, depth - 1);
}

}

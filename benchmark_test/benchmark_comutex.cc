#include <bbt/coroutine/coroutine.hpp>


/**
 * 开启100个协程，竞争写一个全局变量a和b
 * 每个协程每次加1，a和b的值应该始终相等
 * 如果a和b不相等，则说明有协程在写的时候被切换了
 */

using namespace bbt::coroutine::sync;
const int nco_num = 100;
auto mutex = StdLockWapper{bbtco_make_comutex()};

int a = 0;
int b = 0;

int main()
{

    g_scheduler->Start();

    for (int i = 0; i < nco_num; ++i) {
        bbtco []() {
            while (true) {
                std::lock_guard<StdLockWapper> lock(mutex);
                Assert(a == b);
                a++; b++;
            }
        };
    }
    
    while(1) {
        printf("%d %d\n", a, b);
        sleep(1);
    }

}
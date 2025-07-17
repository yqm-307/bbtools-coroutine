#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void FixSizeChan()
{
    auto chan = sync::Chan<int, 1>{};

    /**
     * 展示了chan的基本用法和特征。
     * 
     * 需要注意读写都可能导致当前协程挂起。
     * 
     * 读挂起：当chan中没有可读数据时，当前协程会挂起，直到有可读数据。
     * 写挂起：当chan中没有可写空间时，当前协程会挂起，直到有可写空间。
     * 
     */

    bbtco_ref {
        printf("co_%lu write time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        chan << 1;
        printf("co_%lu write succ time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());

        /**
         * 此时，会阻塞并且挂起。直到chan可写
         */
        printf("co_%lu write time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        chan << 1;
        printf("co_%lu write succ time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
    };

    bbtco_ref {
        bbtco_sleep(100);
        int value = 0;
        printf("co_%lu read time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        chan >> value;
        printf("co_%lu read succ time<%ld>, value: %d\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>(), value);
    };

    sleep(1);

}

void MultiWrite()
{
    /**
     * chan多写挂起：
     * 
     * 对一个满的chan进行的写会导致全部挂起，唤醒不保证顺序，随机唤醒
     */
    auto chan = sync::Chan<int, 1>{};

    for (int i = 0; i < 5; ++i)
    {
        bbtco_ref {
            printf("co_%lu write time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
            chan << 1;
            printf("co_%lu write succ time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        };
    }

    bbtco_ref {
        bbtco_sleep(200);
        for (int i = 0; i < 5; ++i)
        {
            int value = 0;
            chan >> value;
            printf("co_%lu read succ time<%ld>, value: %d\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>(), value);
        }
    };

    sleep(1);
}

void CloseNotify()
{

    // 创建一个协程
    bbtco [](){
        sync::Chan<char, 0> c;
        // 创建协程写入并关闭，此时会唤醒阻塞在Read操作上的协程
        bbtco_ref {
            printf("co2 beg %ld\n", bbt::core::clock::gettime<>());

            /**
             * 保证read操作已经挂起
             * 
             * close后会将所有挂起的读操作唤醒，
             * 并且会将所有挂起的写操作唤醒。
             */
            bbtco_sleep(200);
            c.Close();
            printf("co2 close chan end %ld\n", bbt::core::clock::gettime<>());
        };

        bbtco_ref {
            printf("co3 beg %ld\n", bbt::core::clock::gettime<>());
            /**
             * 读取一个字符，挂起直到关闭或者可读
             * 
             * 注意：如果没有可读元素，Read会挂起当前协程，
             * 直到有可读元素或者chan被关闭。
             */
            c << 10;
            printf("co3 write succ %ld\n", bbt::core::clock::gettime<>());
        };

        /**
         * 因为没有可读元素，所以挂起直到关闭或者可读
         */
        c << 10;
        printf("co1 end %ld\n", bbt::core::clock::gettime<>());
    };

    sleep(1);
}

void NoCacheChan()
{
    // 创建一个无缓存的chan
    auto chan = sync::Chan<int, 0>{};

    bbtco_ref {
        printf("co_%lu write time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        chan << 1;
        printf("co_%lu write succ time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
    };

    bbtco_ref {
        sleep(100);
        int value = 0;
        printf("co_%lu read time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        chan >> value;
        printf("co_%lu read succ time<%ld>, value: %d\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>(), value);
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    printf("=============== fixsize chan ================\n");
    FixSizeChan();
    printf("=============== multi write chan ================\n");
    MultiWrite();
    printf("=============== close notify chan ================\n");
    CloseNotify();
    printf("=============== no cache chan ================\n");
    NoCacheChan();
    g_scheduler->Stop();
}
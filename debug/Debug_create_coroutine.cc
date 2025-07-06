#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

using namespace bbt::coroutine::detail;

int main()
{
    std::vector<Coroutine::Ptr> vec;

    while (true)
    {
        vec.push_back(Coroutine::Create(1024, [](){}, true));    
    }

}
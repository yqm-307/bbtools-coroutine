#include <atomic>
#include <memory>

namespace bbt::coroutine::detail
{

class Profiler
{
public:
    typedef std::unique_ptr<Profiler> UPtr;
    static UPtr& GetInstance();

    Profiler() {}
    ~Profiler() {}

    void OnEvent();
    void OnEvent();

private:
    std::atomic_int             m_total_regist_co_count{0}; /* 总注册协程数量 */
    std::atomic_int             m_total_co_done_count{0};   /* 总完成协程数量 */
};

}
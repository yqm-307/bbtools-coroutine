#include <bbt/coroutine/detail/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

class Coroutine:
    public ICoroutine
{
public:
    Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect = true);
    ~Coroutine();

    virtual void Resume() override;
    virtual void Yield() override;
private:
    Context m_context;
};

}
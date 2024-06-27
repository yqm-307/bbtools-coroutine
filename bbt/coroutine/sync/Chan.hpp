// #pragma once
// #include <queue>
// #include <mutex>
// #include <atomic>
// #include <bbt/coroutine/detail/Define.hpp>
// #include <bbt/coroutine/sync/interface/IChan.hpp>
// #include <bbt/coroutine/detail/Coroutine.hpp>

// namespace bbt::coroutine::sync
// {

// class Chan:
//     public IChan<int>
// {
// public:
//     Chan(int max_queue_size = 65535);
//     ~Chan();

//     virtual int                             Write(const ItemType& item) override;
//     virtual int                             Read(ItemType& item) override;

//     virtual int                             TryRead(ItemType& item) override;
//     virtual int                             TryRead(ItemType& item, int timeout) override;
//     virtual void                            Close() override;
//     virtual bool                            IsClosed() override;
// protected:
//     int                                     _Wait();
//     int                                     _Notify();
// private:
//     const int                               m_max_size{-1};
//     std::queue<ItemType>                    m_item_queue;
//     std::mutex                              m_item_queue_mutex;
//     volatile ChanStatus                     m_run_status{ChanStatus::CHAN_DEFAUTL};
//     detail::Coroutine::SPtr                 m_wait_co{nullptr};
//     std::mutex                              m_wait_co_mutex;
//     std::atomic_bool                        m_is_reading{false};
// };

// }
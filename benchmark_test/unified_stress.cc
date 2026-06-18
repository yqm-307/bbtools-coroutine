// unified_stress.cc — 统一压测：单进程 6 模块并发
// 用法: ./unified_stress [duration_s] [busy_us] [sleep_us] [--module=X] [--threads=N]

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include "fatigue_metrics.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <thread>
#include <chrono>

// CPU control helpers
static int g_busy_us = 0;
static int g_sleep_us = 0;

// busy-spin for us microseconds (only in coroutine context)
#define cpu_spin(us) do { \
    if((us)>0){volatile uint64_t _s=fatigue::now_us();while(fatigue::now_us()-_s<(uint64_t)(us));} \
} while(0)

// yield to scheduler (replaces bbtco_yield with CPU-aware version)
#define cpu_yield() do { cpu_spin(g_busy_us); bbtco_sleep(1); } while(0)

using namespace bbt::coroutine::sync;

static volatile sig_atomic_t g_running=1;

static fatigue::Metrics g_mt_comutex("comutex");
static fatigue::Metrics g_mt_corwmutex("corwmutex");
static fatigue::Metrics g_mt_cocond("cocond");
static fatigue::Metrics g_mt_chan("chan");
static fatigue::Metrics g_mt_copool("copool");
static fatigue::Metrics g_mt_coroutine("coroutine");

// 全局状态(bbtco_ref [&] 可安全捕获)
static std::shared_ptr<CoMutex>   g_cs_m;    static volatile int g_cs_a,g_cs_b;
static std::shared_ptr<CoRWMutex> g_rs_rw;   static volatile int g_rs_val;
static std::shared_ptr<CoCond>    g_ds_cond; static std::unique_ptr<std::atomic_int[]> g_ds_alive;
static std::atomic_int g_ds_frame{0}; static const int g_ds_nco=200;
static Chan<uint64_t,1000> g_ch_buf; static Chan<uint64_t,0> g_ch_nb;
static std::shared_ptr<bbt::coroutine::pool::CoPool> g_ps_pool;

void on_signal(int){g_running=0;}
static void stop_all(){g_mt_comutex.stop_reporter();g_mt_corwmutex.stop_reporter();g_mt_cocond.stop_reporter();g_mt_chan.stop_reporter();g_mt_copool.stop_reporter();g_mt_coroutine.stop_reporter();}

static void stress_comutex(){
    g_cs_m=bbtco_make_comutex();g_cs_a=g_cs_b=0;
    for(int i=0;i<40;++i)bbtco_ref{while(g_running){uint64_t t0=fatigue::now_us();g_cs_m->Lock();uint64_t lat=fatigue::now_us()-t0;g_mt_comutex.lock_ops++;g_mt_comutex.record_lock_latency_us(lat);Assert(g_cs_a==g_cs_b);g_cs_a++;g_cs_b++;g_mt_comutex.ops_total++;g_cs_m->UnLock();cpu_yield();};};
    for(int i=0;i<10;++i)bbtco_ref{while(g_running){uint64_t t0=fatigue::now_us();int ret=g_cs_m->TryLock(10);uint64_t lat=fatigue::now_us()-t0;if(ret==0){g_mt_comutex.trylock_success++;g_mt_comutex.record_lock_latency_us(lat);Assert(g_cs_a==g_cs_b);g_cs_a++;g_cs_b++;g_mt_comutex.ops_total++;g_cs_m->UnLock();}else g_mt_comutex.trylock_timeout++;cpu_yield();};};
}

static void stress_corwmutex(){
    g_rs_rw=bbtco_make_corwmutex();g_rs_val=0;
    for(int i=0;i<30;++i)bbtco_ref{while(g_running){g_rs_rw->RLock();g_mt_corwmutex.rlock_ops++;g_mt_corwmutex.ops_total++;volatile int v=g_rs_val;(void)v;g_rs_rw->RUnLock();cpu_yield();};};
    for(int i=0;i<5;++i)bbtco_ref{while(g_running){uint64_t t0=fatigue::now_us();g_rs_rw->WLock();uint64_t lat=fatigue::now_us()-t0;g_mt_corwmutex.wlock_ops++;g_mt_corwmutex.ops_total++;g_mt_corwmutex.record_lock_latency_us(lat);g_rs_val++;g_rs_rw->WUnLock();cpu_yield();};};
    for(int i=0;i<3;++i)bbtco_ref{while(g_running){if(g_rs_rw->TryWLock(5)==0){g_mt_corwmutex.wlock_ops++;g_mt_corwmutex.ops_total++;g_rs_val++;g_rs_rw->WUnLock();}else g_mt_corwmutex.try_wlock_timeout++;cpu_yield();};};
}

static void stress_cocond(){
    g_ds_cond=bbtco_make_cocond();g_ds_alive.reset(new std::atomic_int[g_ds_nco]());g_ds_frame=0;
    for(int i=0;i<g_ds_nco;++i)bbtco_ref{int _i=i;while(g_running){g_ds_alive[_i].store(g_ds_frame.load());g_ds_cond->Wait();g_mt_cocond.cond_waits++;g_mt_cocond.ops_total++;};};
    bbtco_ref{while(g_running){g_ds_cond->NotifyAll();g_mt_cocond.cond_signals++;int lost=0;for(int i=0;i<g_ds_nco&&g_running;++i)if(g_ds_frame-g_ds_alive[i]>10){lost++;g_mt_cocond.errors++;}if(lost)printf("[cocond] WARN:%d lost\n",lost);g_ds_frame++;bbtco_sleep(500);};};
}

static void stress_chan(){
    for(int i=0;i<5;++i)bbtco_ref{uint64_t v=0;while(g_running){g_ch_buf.Write(v++);g_mt_chan.chan_writes++;cpu_yield();g_mt_chan.ops_total++;};};
    for(int i=0;i<10;++i)bbtco_ref{uint64_t v;while(g_running){g_ch_buf.Read(v);g_mt_chan.chan_reads++;cpu_yield();g_mt_chan.ops_total++;};};
    for(int i=0;i<3;++i)bbtco_ref{uint64_t v=0;while(g_running){g_ch_nb.Write(v++);g_mt_chan.chan_writes++;cpu_yield();g_mt_chan.ops_total++;};};
    for(int i=0;i<3;++i)bbtco_ref{uint64_t v;while(g_running){g_ch_nb.Read(v);g_mt_chan.chan_reads++;cpu_yield();g_mt_chan.ops_total++;};};
}

static void stress_chan_close(){
    // 测试 Close 后读缓冲语义：Close 不应丢弃数据，读空后才返回 -1
    static Chan<uint64_t,500> g_close_buf;
    static std::atomic_bool g_phase1_done{false};
    static const int n_writers=3, n_readers=5, n_fill=400;

    // Phase 1: 填充缓冲 → Close → 读者应能读完
    bbtco_ref{
        for(int i=0;i<n_fill;++i) g_close_buf.Write(i);
        g_close_buf.Close();
        g_phase1_done=true;
    };
    bbtco_ref{
        while(!g_phase1_done) bbtco_sleep(1);
        uint64_t v; int n=0; int ret=0;
        while((ret=g_close_buf.Read(v))==0){
            n++; g_mt_chan.ops_total++;
        }
        // ret == -1: 关闭+空 ✅
        if(ret==-1) g_mt_chan.chan_reads++;
        else g_mt_chan.errors++;
    };
}

static void stress_copool(){
    g_ps_pool=bbtco_make_copool(20);
    bbtco_ref{while(g_running){g_ps_pool->Submit([](){volatile int x=0;for(int j=0;j<100;++j)x+=j;});g_mt_copool.pool_tasks++;g_mt_copool.ops_total++;bbtco_sleep(1);};};
}

static void stress_coroutine(){
    for(int i=0;i<5;++i)bbtco_ref{while(g_running){bbtco [](){g_mt_coroutine.co_spawns++;g_mt_coroutine.ops_total++;};bbtco_sleep(10);};};
}

int main(int argc,char**argv){
    int dur=3600;
    const char* module=nullptr;
    int proc_threads=0;
    int pos=0; // positional arg index

    for(int i=1;i<argc;++i){
        if(strncmp(argv[i],"--module=",9)==0) module=argv[i]+9;
        else if(strncmp(argv[i],"--threads=",10)==0) proc_threads=atoi(argv[i]+10);
        else switch(pos++){
            case 0: dur=atoi(argv[i]); break;
            case 1: g_busy_us=atoi(argv[i]); break;
            case 2: g_sleep_us=atoi(argv[i]); break;
        }
    }

    const char*te=getenv("CO_PROC_THREADS");
    if(te&&proc_threads==0) proc_threads=atoi(te);
    if(proc_threads>0){
        g_bbt_coroutine_config->m_cfg_static_thread_num=(size_t)proc_threads;
    }

    printf("[unified_stress] dur=%ds busy=%dus sleep=%dus threads=%zu",
        dur,g_busy_us,g_sleep_us,g_bbt_coroutine_config->m_cfg_static_thread_num);
    if(module) printf(" module=%s",module);
    printf("\n");

    int ival=60;const char*e=getenv("FATIGUE_INTERVAL");if(e)ival=atoi(e);
    printf("[unified_stress] interval=%ds\n",ival);
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);
    g_scheduler->Start();

#define RUN_MOD(m) if(!module||strcmp(module,#m)==0) stress_##m()
    RUN_MOD(comutex);
    RUN_MOD(corwmutex);
    RUN_MOD(cocond);
    RUN_MOD(chan);
    RUN_MOD(chan_close);
    RUN_MOD(copool);
    RUN_MOD(coroutine);
#undef RUN_MOD

    g_mt_comutex.start_reporter(ival);
    g_mt_corwmutex.start_reporter(ival);
    g_mt_cocond.start_reporter(ival);
    g_mt_chan.start_reporter(ival);
    g_mt_copool.start_reporter(ival);
    g_mt_coroutine.start_reporter(ival);

    printf("[unified_stress] started\n");
    for(int i=0;i<dur&&g_running;++i){
        std::this_thread::sleep_for(std::chrono::seconds(1));
#ifdef BBT_COROUTINE_PROFILE
        if (i % 10 == 0) g_bbt_profiler->DumpStderr();
#endif
    }
    g_running=0;std::this_thread::sleep_for(std::chrono::seconds(1));
    stop_all();g_scheduler->Stop();
    printf("[unified_stress] done\n");
    return 0;
}




#ifndef __CPU_O3_FETCH_TOP_HH__
#define __CPU_O3_FETCH_TOP_HH__

#include "cpu/o3/fetch.hh"
#include "cpu/o3/fetch_stage.hh"

namespace gem5
{
struct BaseO3CPUParams;

namespace o3
{
class CPU;

class FetchTop
{
  public:
    FetchTop(CPU *_cpu, const BaseO3CPUParams &params);
    std::function<void (void)> tick;
    std::function<void (void)>  startupStage;
    std::function<void (ThreadID tid)>  clearStates;
    std::function<RequestPort& (void)>  getInstPort;
    std::function<void (ThreadID tid)>  deactivateThread;
    std::function<void (std::list<ThreadID> *at_ptr)>  setActiveThreads;
    std::function<void (TimeBuffer<TimeStruct> *time_buffer)>  setTimeBuffer;
    std::function<void (TimeBuffer<FetchStruct> *fq_ptr)>  setFetchQueue;
    std::function<void (void)>  regProbePoints;
    std::function<void (void)>  wakeFromQuiesce;
    std::function<void (void)>  drainSanityCheck;
    std::function<bool (void)>  isDrained;
    std::function<void (ThreadID tid)>  drainStall;
    std::function<void (void)>  drainResume;
    std::function<void (void)>  takeOverFrom;
    std::function<void (void)>  flushFetchBuffer;

  public:
    InstDecoder *decoder[MaxThreads];

  private:
    std::shared_ptr<FetchStage> pFetchStage = nullptr;
    std::shared_ptr<Fetch> pFetch = nullptr;
};
}
}

#endif
#include "cpu/o3/dyn_inst.hh"
#include "debug/Fetch.hh"
#include "fetch_top.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{
FetchTop::FetchTop(CPU *_cpu, const BaseO3CPUParams &params)
{
    if (params.enable_decoupledFrontend) {
        std::cout << "use decoupled frontend " << std::endl;
        pFetchStage = std::make_shared<FetchStage>(_cpu, params);
        FetchStage& fetch = *pFetchStage;
        for (auto ii=0; ii<MaxThreads; ii++) {
          decoder[ii] = fetch.decoder[ii];
        }
        tick = [&](void) {fetch.tick();};
        startupStage = [&](void) {fetch.startupStage();};
        clearStates =  [&](ThreadID tid) {fetch.clearStates(tid);};
        getInstPort = [&](void)->RequestPort& {return fetch.getInstPort();};
        deactivateThread = [&](ThreadID tid) {fetch.deactivateThread(tid);};
        setActiveThreads = [&](std::list<ThreadID> *at_ptr) {
            fetch.setActiveThreads(at_ptr);};
        setTimeBuffer = [&](TimeBuffer<TimeStruct> *time_buffer) {
            fetch.setTimeBuffer(time_buffer);};
        setFetchQueue = [&](TimeBuffer<FetchStruct> *fq_ptr) {
            fetch.setFetchQueue(fq_ptr);};
        regProbePoints = [&](void) {fetch.regProbePoints();};
        wakeFromQuiesce = [&](void) {fetch.wakeFromQuiesce();};
        drainSanityCheck = [&](void) {fetch.drainSanityCheck();};
        isDrained = [&](void)->bool {return fetch.isDrained();};
        drainStall = [&](ThreadID tid) {fetch.drainStall(tid);};
        drainResume = [&](void) {fetch.drainResume();};
        takeOverFrom = [&](void) {fetch.takeOverFrom();};
        flushFetchBuffer = [&](void) {fetch.flushFetchBuffer();};
    } else {
        std::cout << "use GEM5 frontend " << std::endl;
        pFetch = std::make_shared<Fetch>(_cpu, params);
        Fetch& fetch = *pFetch;
        tick = [&](void) {fetch.tick();};
        startupStage = [&](void) {fetch.startupStage();};
        clearStates =  [&](ThreadID tid) {fetch.clearStates(tid);};
        getInstPort = [&](void)->RequestPort& {return fetch.getInstPort();};
        deactivateThread = [&](ThreadID tid) {fetch.deactivateThread(tid);};
        setActiveThreads = [&](std::list<ThreadID> *at_ptr) {
            fetch.setActiveThreads(at_ptr);};
        setTimeBuffer = [&](TimeBuffer<TimeStruct> *time_buffer) {
            fetch.setTimeBuffer(time_buffer);};
        setFetchQueue = [&](TimeBuffer<FetchStruct> *fq_ptr) {
            fetch.setFetchQueue(fq_ptr);};
        regProbePoints = [&](void) {fetch.regProbePoints();};
        wakeFromQuiesce = [&](void) {fetch.wakeFromQuiesce();};
        drainSanityCheck = [&](void) {fetch.drainSanityCheck();};
        isDrained = [&](void)->bool {return fetch.isDrained();};
        drainStall = [&](ThreadID tid) {fetch.drainStall(tid);};
        drainResume = [&](void) {fetch.drainResume();};
        takeOverFrom = [&](void) {fetch.takeOverFrom();};
        flushFetchBuffer = [&](void) {fetch.flushFetchBuffer();};
    }
}
}
}

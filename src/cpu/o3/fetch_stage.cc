/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fetch_stage.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "arch/riscv/pcstate.hh"
#include "base/random.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

FetchStage::IcachePort::IcachePort(FetchStage *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}


FetchStage::FetchStage(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      toBpuQueue(params.backComSize, params.forwardComSize),
      toIfuQueue(params.backComSize, params.forwardComSize),
      f0Queue(params.backComSize, params.forwardComSize),
      f1Queue(params.backComSize, params.forwardComSize),
      f2Queue(params.backComSize, params.forwardComSize),
      decodeInstQueue(params.backComSize, params.forwardComSize),
      branchPred(nullptr),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      finishTranslationEvent(this),
      fetchStats(_cpu, this)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    if (fetchBufferSize > cacheBlkSize)
        fatal("fetch buffer size (%u bytes) is greater than the cache "
              "block size (%u bytes)\n", fetchBufferSize, cacheBlkSize);
    if (cacheBlkSize % fetchBufferSize)
        fatal("cache block (%u bytes) is not a multiple of the "
              "fetch buffer (%u bytes)\n", cacheBlkSize, fetchBufferSize);

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        fetchOffset[i] = 0;
        preFtqInfoEndAddr[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        memReq[i].clear();
        stalls[i] = {false, false};
        fetchBufferPC[i] = 0;
        fetchBufferValid[i] = false;
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
    }

    branchPred = (DecoupledBPU *)params.decoupledBPU;
    enInstPrefetch= params.instPrefetch;

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
    }

    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();

    // set timebuffer wire between fetch stages
    int delayOneCycle = 1;
    toF1 = f0Queue.getWire(0);
    fromF0 = f0Queue.getWire(-delayOneCycle);
    toF2 = f1Queue.getWire(0);
    fromF1 = f1Queue.getWire(-delayOneCycle);
    toF3 = f2Queue.getWire(0);
    fromF2 = f2Queue.getWire(-delayOneCycle);
    toF3Insts = decodeInstQueue.getWire(0);
    fromF2Insts = decodeInstQueue.getWire(-delayOneCycle);


    // set timebuffer wire between IFU and BPU
    bpuToIfuDelay = 1; // FIXME, from params
    branchPred->setBpuToFetchQueue(&toIfuQueue);
    branchPred->setFetchToBpuQueue(&toBpuQueue);
    toBpu = toBpuQueue.getWire(0);
    // delay to match FTQ squashing
    fromIfu = toBpuQueue.getWire(-bpuToIfuDelay);
    fromBpu = toIfuQueue.getWire(-bpuToIfuDelay);

    ftqInfoBufferMaxSize = bpuToIfuDelay + 1;
    ftb_false_hit = 0;
    prefetchCount = 0;
}

std::string FetchStage::name() const { return cpu->name() + ".fetch_stage"; }

void
FetchStage::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

FetchStage::FetchStatGroup::FetchStatGroup(CPU *cpu, FetchStage *fetch)
    : statistics::Group(cpu, "fetch_stage"),
    ADD_STAT(ftqStallCycles, statistics::units::Cycle::get(),
             "Number of cycles ifu wait ftq cmd"),
    ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch is stalled on an Icache miss"),
    ADD_STAT(insts, statistics::units::Count::get(),
             "Number of instructions fetch has processed"),
    ADD_STAT(branches, statistics::units::Count::get(),
             "Number of branches that fetch encountered"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(cacheLine0_req, statistics::units::Count::get(),
             "Number of fetched cache pipeline0 request"),
    ADD_STAT(cacheLine1_req, statistics::units::Count::get(),
             "Number of fetched cache pipeline1 request"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(branchRate, statistics::units::Ratio::get(),
             "Number of branch fetches per cycle",
             branches / cpu->baseStats.numCycles),
    ADD_STAT(rate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of inst fetches per cycle",
             insts / cpu->baseStats.numCycles)
{
        ftqStallCycles
            .prereq(ftqStallCycles);
        icacheStallCycles
            .prereq(icacheStallCycles);
        insts
            .prereq(insts);
        branches
            .prereq(branches);
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        cacheLine0_req
            .prereq(cacheLine0_req);
        cacheLine1_req
            .prereq(cacheLine1_req);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        branchRate
            .flags(statistics::total);
        rate
            .flags(statistics::total);
}
void
FetchStage::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromDecodeToIfu = timeBuffer->getWire(-decodeToFetchDelay-1);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
    //delay one cycle to match BPU
    fromCommitToIfu = timeBuffer->getWire(-commitToFetchDelay-1);
}

void
FetchStage::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
FetchStage::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);
}

void
FetchStage::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();

    // init BPU start address
    branchPred->setStartAddr(cpu->pcState(0).instAddr());
}

void
FetchStage::clearStates(ThreadID tid)
{
    fetchStatus[tid] = Running;
    set(pc[tid], cpu->pcState(tid));
    fetchOffset[tid] = 0;
    preFtqInfoEndAddr[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid].clear();
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBufferPC[tid] = 0;
    fetchBufferValid[tid] = false;
    fetchQueue[tid].clear();
    idxToOffset.clear();

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
FetchStage::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        fetchStatus[tid] = Running;
        set(pc[tid], cpu->pcState(tid));
        fetchOffset[tid] = 0;
        preFtqInfoEndAddr[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid].clear();

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBufferPC[tid] = 0;
        fetchBufferValid[tid] = false;

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
    }
    idxToOffset.clear();
    wroteToTimeBuffer = false;
    _status = Inactive;
    status_change = false;
}

void
FetchStage::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());

    DPRINTF(Fetch, "[tid:%i] Waking up from cache miss.\n", tid);
    assert(!cpu->switchedOut());

    // Only change the status if it's still waiting on the icache access
    // to return.
    if (!isStatus(pkt->req, IcacheWaitResponse)) {
        bool is_prefetch = pkt->req->isPrefetch();
        if (is_prefetch) {
          assert(prefetchCount > 0);
          DPRINTF(Fetch, "[tid:%i] prefetch iCache completed, size:%d."
              "req:%x\n", tid, prefetchCount, pkt->req.get());
          prefetchCount--;
        } else {
          ++fetchStats.icacheSquashes;
          DPRINTF(Fetch, "[tid:%i] Ignoring iCache completed after squash."
              "req:%x\n", tid, pkt->req.get());
        }
        eraseReq(pkt->req, FetchReqStatus::Finish);
        delete pkt;
        return;
    }

    DPRINTF(Fetch, "[tid:%i] memcpy cacheline, mem_req:%x,"
        " fetchBuffer size:%d.\n",
        tid, pkt->req.get(), fetchBuffer[tid].size());

    // Create space to buffer the cache line data,
    uint8_t* buffer = new uint8_t[fetchBufferSize];
    memset(buffer, 0, sizeof(uint8_t) * fetchBufferSize);
    fetchBuffer[tid].emplace(std::make_pair(pkt->req, buffer));
    memcpy(buffer, pkt->getConstPtr<uint8_t>(), fetchBufferSize);
    assert(fetchBuffer[tid].size() < 7);

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Only switch to IcacheAccessComplete if we're not stalled as well.
    if (checkStall(tid)) {
        fetchStatus[tid] = Blocked;
    } else {
        setReqStatus(pkt->req, IcacheAccessComplete);
    }

    pkt->req->setAccessLatency();
    cpu->ppInstAccessComplete->notify(pkt);
    // Reset the mem req to NULL.
    delete pkt;
}

void
FetchStage::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].decode = false;
        stalls[i].drain = false;
    }
}

void
FetchStage::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPktQ.empty());
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(memReq[i].size() > 0);
        assert(fetchStatus[i] == Idle || stalls[i].drain);
    }

    //branchPred->drainSanityCheck();
}

bool
FetchStage::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            if (fetchStatus[i] == Blocked && stalls[i].drain)
                continue;
            else
                return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !finishTranslationEvent.scheduled();
}

void
FetchStage::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
FetchStage::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

void
FetchStage::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    fetchStatus[0] = Running;
}

void
FetchStage::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
FetchStage::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
FetchStage::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
FetchStage::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken = false;

    if (!inst->isControl()) {
        inst->staticInst->advancePC(next_pc);
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    // ThreadID tid = inst->threadNumber;
    // predict_taken = branchPred->predict(inst->staticInst, inst->seqNum,
    //                                     next_pc, tid);

    // if (predict_taken) {
    //     DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
    //             "predicted to be taken to %s\n",
    //             tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    // } else {
    //     DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
    //             "predicted to be not taken\n",
    //             tid, inst->seqNum, inst->pcState().instAddr());
    // }

    // DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
    //         "predicted to go to %s\n",
    //         tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    // inst->setPredTarg(next_pc);
    // inst->setPredTaken(predict_taken);

    // ++fetchStats.branches;

    // if (predict_taken) {
    //     ++fetchStats.predictedBranches;
    // }

    return predict_taken;
}

RequestPtr
FetchStage::fetchCacheLine(Addr vaddr, ThreadID tid,
                          const PCStateBase &this_pc,
                          bool is_prefetch)
{
    Addr pc = this_pc.instAddr();
    Fault fault = NoFault;

    assert(!cpu->switchedOut());

    // @todo: not sure if these should block translation.
    //AlphaDep
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n",
                tid);
        return nullptr;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n",
                tid);
        return nullptr;
    }

    // Align the fetch address to the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(vaddr);

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for PC %s\n",
            tid, fetchBufferBlockPC, this_pc);

    // Build request here.
    Request::FlagsType flag_type = !is_prefetch ? Request::INST_FETCH
                                                : Request::PREFETCH;
    RequestPtr mem_req = std::make_shared<Request>(
        fetchBufferBlockPC, fetchBufferSize,
        flag_type, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

#if 0 // TODO
    if (enInstPrefetch && is_prefetch) {
      mem_req->setCacheOrgId(0);// id=0:iCache
                                // TODO, from icache->getCacheOrgId()
      mem_req->setPrefetchTgtId(2); // id=2:L2
    }
#endif

    mem_req->taskId(cpu->taskId());
    pushReqToList(mem_req, this_pc);
    DPRINTF(Fetch, "[tid:%i] Fetching cache line new req: %x\n",
            tid, mem_req.get());
    // Initiate translation of the icache block
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);
    return mem_req;
}

void
FetchStage::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    //Addr fetchBufferBlockPC = mem_req->getVaddr();

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    DPRINTF(Fetch, " finishTranslation memReq:%x.\n", mem_req.get());
    if (!isStatus(mem_req, FetchReqStatus::ItlbWait)) {
        DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash\n",
                tid);
        eraseReq(mem_req, FetchReqStatus::Finish);
        ++fetchStats.tlbSquashes;
        return;
    }
    bool is_prefetch = mem_req->isPrefetch();

    // If translation was successful, attempt to read the icache block.
    if (fault == NoFault) {
        // Check that we're not going off into random memory
        // If we have, just wait around for commit to squash something and put
        // us on the right track
        if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
            warn("Address %#x is outside of physical memory, stopping fetch\n",
                    mem_req->getPaddr());
            setReqStatus(mem_req, NoGoodAddr);
            //memReq[tid] = NULL;
            return;
        }

        // Build packet here.
        MemCmd cmd = is_prefetch ? MemCmd::HardPFReq : MemCmd::ReadReq;
        PacketPtr data_pkt = new Packet(mem_req, cmd);
        data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);

        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

        fetchStats.cacheLines++;

        // Access the cache.
        if (!icachePort.sendTimingReq(data_pkt)) {
            DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);
            if (is_prefetch) {
                eraseReq(mem_req, FetchReqStatus::Finish);
            } else {
                assert(retryPktQ.size() <= 2);
                setReqStatus(mem_req, IcacheWaitRetry);
                retryPktQ.push(data_pkt);
                retryTid = tid;
                cacheBlocked = true;
            }
        } else {
            DPRINTF(Fetch, "[tid:%i] Doing Icache access, req:%x.\n",
                tid, mem_req.get());
            DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                    "response.\n", tid);
            lastIcacheStall[tid] = curTick();
            if (!is_prefetch) {
                setReqStatus(mem_req, IcacheWaitResponse);
            } else {
                eraseReq(mem_req, FetchReqStatus::Finish);
            }
            // Notify Fetch Request probe when a packet containing a fetch
            // request is successfully sent
            ppFetchRequestSent->notify(mem_req);
        }
    } else if (is_prefetch) {
        setReqStatus(mem_req, FetchReqStatus::Finish);
        DPRINTF(Fetch, "[tid:%i] prefetch fault (%s) detected @ PC %s.\n",
                tid, fault->name(), getReqPc(mem_req));
    } else {
        setReqStatus(mem_req, FetchReqStatus::ItlbFault);
        setReqFault(mem_req, fault);
        fetchBuffer[tid].emplace(std::make_pair(mem_req, nullptr));

        DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
        DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
                tid, fault->name(), getReqPc(mem_req));
    }
    _status = updateFetchStatus();
}
void
FetchStage::intraSquash(ThreadID tid, std::vector<unsigned>& squashed_ftq_idxs)
{
    for (auto idx : squashed_ftq_idxs) {
        DPRINTF(Fetch, "intra_squash, ftqIdx:%i \n", idx);
        if (ftqIdxReqs.find(idx) != ftqIdxReqs.end()) {
            auto& reqs = ftqIdxReqs.at(idx);
            setReqStatus(reqs, FetchReqStatus::Squashed);
            DPRINTF(Fetch, "squashed req:%x\n", reqs.at(0).get());
        }
        auto it=ftqFetchInfoBuffer.begin();
        while ( it!=ftqFetchInfoBuffer.end()) {
            DPRINTF(Fetch, "fetchInfo idx:%i\n", it->ftqIdx);
            if (idx == it->ftqIdx) {
                it = ftqFetchInfoBuffer.erase(it);
                // fetchReq associated with ftqFetchInfoBuffer
                // need to clear together
                if ((fetchReq[tid].size() > 0)
                    && (idx == getReqFetchInfo(fetchReq[tid].at(0)).ftqIdx)) {
                    fetchReq[tid].clear();
                    DPRINTF(Fetch, "clear fetchReq\n");
                }
                DPRINTF(Fetch, "squashed idx:%i\n", idx);
            } else {
                it++;
            }
        }

        // squash retryPkt req
        while (!retryPktQ.empty()) {
            auto retry_pkt = retryPktQ.front();
            auto retry_req = retry_pkt->req;
            if (idx == getReqFetchInfo(retry_req).ftqIdx) {
                DPRINTF(Fetch, "squashed retry req:%x\n", retry_req.get());
                delete retry_pkt;
                retry_pkt = nullptr;
                retryPktQ.pop();
            } else {
                break;
            }
        };

        if (retryPktQ.empty()) {
            retryTid = InvalidThreadID;
        }
    }
    updateIcacheDataMap(); // called before updateReqList()
    updateReqList();
    // revert fetchOffset for predecode
    auto ftq_idx = squashed_ftq_idxs.at(0); // 1st squashed idx
    if (idxToOffset.find(ftq_idx) != idxToOffset.end()) {
      fetchOffset[tid] = idxToOffset.at(ftq_idx).first;
      preFtqInfoEndAddr[tid] = idxToOffset.at(ftq_idx).second;
      idxToOffset.erase(ftq_idx);
      DPRINTF(Fetch, "revert ftqIdx:%d fetchOffset to:%d\n",
          ftq_idx, fetchOffset[tid]);
    }
}
void
FetchStage::tick0Squash(void)
{
    // clear ftq info
    setSquash(fromBpu);
    while (!ftqFetchInfoBuffer.empty()) {
        ftqFetchInfoBuffer.pop_front();
    }
    // squash req
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        setReqStatus(fetchReq[tid], FetchReqStatus::Squashed);
        fetchReq[tid].clear();
    }
    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == 0) {
        assert(cacheBlocked);
        while (!retryPktQ.empty())
        {
          auto retry_pkt = retryPktQ.front();
          delete retry_pkt;
          retry_pkt = NULL;
          retryPktQ.pop();
        }
        retryTid = InvalidThreadID;
    }
}
void
FetchStage::tick1Squash(void)
{
    setSquash(fromF0);
}
void
FetchStage::tick2Squash(void)
{
    setSquash(fromF1);
    for (ThreadID i = 0; i < numThreads; ++i) {
        setReqStatus(memReq[i], FetchReqStatus::Squashed);
        clear(fetchBuffer[i]);
    }
    idxToOffset.clear();
    fetchOffset[0] = 0;
    preFtqInfoEndAddr[0] = 0;
    decoder[0]->reset();
}
void
FetchStage::tick3Squash(void)
{
    setSquash(fromF2);
    setSquash(fromF2Insts);
}

void
FetchStage::fetchQueueSquash(void)
{
    // clear fetch queue
    for (ThreadID i = 0; i < numThreads; ++i) {
        for (auto& inst : fetchQueue[i]) {
            inst->setSquashed();
        }
        fetchQueue[i].clear();
    }
}

void
FetchStage::doSquash(const PCStateBase &new_pc, const DynInstPtr squashInst,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s.\n",
            tid, new_pc);

    fetchStatus[tid] = Squashing;

    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;

    // squash every tick
    tick0Squash();
    tick1Squash();
    tick2Squash();
    tick3Squash();
    updateReqList();
    ++fetchStats.squashCycles;
}

void
FetchStage::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        fetchBufferValid[i] = false;
    }
}

bool
FetchStage::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(Fetch,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

FetchStage::FetchStatus
FetchStage::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == Squashing) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);
                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
FetchStage::tick()
{
    bool status_change = checkSignalsAndUpdate();

    tick3();
    tick2();
    tick1();
    tick0();
    branchPred->tick();

    // feedback precheck result to FTQ
    sendInfoToBpu();

    // advance time buffer
    advance();

    // update flag
    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }
}

void
FetchStage::sendInfoToBpu()
{
    // fetch block bpu
    toBpu->bpuBlock = f0BlockFlag;

    // precheck result
    toBpu->preDecSquash = preDecSquash;
    toBpu->pdInfo = pdInfo;

    auto& commit_info = fromCommit->commitInfo[0];
    auto& decode_info = fromDecode->decodeInfo[0];
    // commit insts from backend
    toBpu->cmtInfo = commit_info.cmtInfo;
    toBpu->interruptPending = commit_info.interruptPending;
    toBpu->clearInterrupt = commit_info.clearInterrupt;

    // squash from backend
    if (commit_info.squash) {
        toBpu->backendSquash.squash = true;
        toBpu->backendSquash.squashInst = commit_info.squashInst;
        toBpu->backendSquash.mispredictInst = commit_info.mispredictInst;
        toBpu->backendSquash.branchTaken = commit_info.branchTaken;
        toBpu->backendSquash.squashItSelf = commit_info.squashItSelf;
        set(toBpu->backendSquash.pc, commit_info.pc);
    } else if (decode_info.squash) {
        toBpu->backendSquash.squash = true;
        toBpu->backendSquash.squashInst = decode_info.squashInst;
        toBpu->backendSquash.mispredictInst = decode_info.mispredictInst;
        toBpu->backendSquash.branchTaken = commit_info.branchTaken;
        set(toBpu->backendSquash.pc, decode_info.nextPC);
    } else {
        toBpu->backendSquash.squash = false;
    }
}

void
FetchStage::tick1()
{
    DPRINTF(Fetch, "Running F1 stage.\n");
    f1BlockFlag = false;
    if (isBackPressure<std::vector<RequestPtr>>(f1Queue)) {
        DPRINTF(Fetch, "F1 back pressured.\n");
        f1BlockFlag = true;
        return;
    }
    std::vector<RequestPtr> &mem_reqs = *fromF0;
    if (!isValid(mem_reqs)) {
        (*fromF0).clear();
        return;
    } else if (!isEmpty(mem_reqs)) {
        DPRINTF(Fetch, "F1 read req:%x and send to F2.\n",
                mem_reqs.at(0).get());
        *toF2 = mem_reqs;
        (*fromF0).clear();
    }
}

void
FetchStage::tick2()
{
    DPRINTF(Fetch, "Running F2 stage.\n");
    f2BlockFlag = false;
    if (!isValid(fromF1)) {
        return;
    }

    ThreadID tid = 0;
    memReq[tid] = *fromF1;
    if (hasStatus(memReq[tid], IcacheWaitResponse)) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    }

    if (isBackPressure<std::vector<RequestPtr>>(f2Queue)) {
        DPRINTF(Fetch, "F2 back pressured.\n");
        f2BlockFlag = true;
        return;
    }

    memReq[tid] = *fromF1;
    DPRINTF(Fetch, "tick2 read req:%x.\n", memReq[tid].at(0).get());
    if (!isValid(memReq[tid])) {
        (*fromF1).clear();
        return;
    }

    // init
    status_change = false;
    numInst = 0;
    fetchInstQueue.clear();

    // get icacheline and decode
    FtqFetchInfo& fetch_info = getReqFetchInfo(memReq[tid].at(0));
    unsigned ftq_idx = fetch_info.ftqIdx;
    predecode(memReq[tid], ftq_idx);

    if (fetchInstQueue.size() > 0) {
        (*fromF1).clear();

        // send inst to ibuffer
        *toF3 = memReq[tid];
        for (auto& inst : fetchInstQueue) {
            toF3Insts->push_back(inst);
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to F3\n",
                    tid, inst->seqNum);
        }
    }
    DPRINTF(Fetch, "toF3Insts->size:%d\n", toF3Insts->size());
}

void
FetchStage::tick3()
{
    // init
    uint32_t tid = 0;
    preDecSquash.squash = false;
    pdInfo.valid = false;

    // send inst to fetch queue
    f3BlockFlag = false;
    assert(fetchQueue[0].size() <= fetchQueueSize);
    std::vector<DynInstPtr> precode_insts;
    auto& reqs = (*fromF2);
    if (!isValid(reqs) && (reqs.size() > 0)) {
        tick3Squash();
    } else if (fromF2Insts->size() > 0) {
        DPRINTF(Fetch, "fetchQueue[0].size() :%d\n", fetchQueue[0].size());
        if ((fetchQueue[0].size() + fromF2Insts->size()) <= fetchQueueSize) {
            precode_insts = *fromF2Insts;
            fromF2Insts->clear();

            // check precode insts
            auto& ftq_info = getReqFetchInfo(reqs.at(0));
            std::vector<DynInstPtr> valid_insts;
            std::vector<DynInstPtr> invalid_insts;
            precheck(ftq_info, precode_insts, valid_insts, invalid_insts);
            // squash predecode insts after precheck
            // predecodeSquash(valid_insts);

            // push to fetchQueue
            for (auto& inst : valid_insts) {
                fetchQueue[0].push_back(inst);
                // Increment stat of fetched instructions.
                ++fetchStats.insts;
                DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction "
                    "to FetchQ, size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());
            }
            // Record number of instructions fetched this cycle
            // for distribution.
            fetchStats.nisnDist.sample(valid_insts.size());

            // remove invalid inst
            for (auto& inst : invalid_insts) {
                DPRINTF(Fetch, "[tid:%i] [sn:%llu] rm invalid instruction.\n",
                    tid, inst->seqNum);
                if (inst->isSquashed()) {
                    continue;
                }
                cpu->squashAndremoveInst(inst);
            }

            // finish req
            idxToOffset.erase(ftq_info.ftqIdx);
            eraseReq(reqs, FetchReqStatus::Finish);
            (*fromF2).clear();
        } else {
            f3BlockFlag = true;
        }
    }

    // send inst from queue to decode stage
    sendInstToDecode();

    // If there was activity this cycle, inform the CPU of it.
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}
void
FetchStage::precheck(FtqFetchInfo& fetch_info, std::vector<DynInstPtr>& insts,
                std::vector<DynInstPtr>& valid_insts,
                std::vector<DynInstPtr>& invalid_insts)
{
    Addr start_pc = fetch_info.startAddr;
    Addr end_pc = fetch_info.endAddr;
    Addr target = fetch_info.target;
    uint32_t cfi_idx = fetch_info.cfiIndex;
    uint32_t ftq_idx = fetch_info.ftqIdx;
    bool pred_taken = fetch_info.predTaken;
    uint32_t fault_idx = (uint32_t)gem5::o3::MaxInstrsPerBlock;

    if (pred_taken) {
        ++fetchStats.predictedBranches;
    }

    DPRINTF(Fetch, "precheck ftq_idx:%d, startAddr:%x,"
        "cfi_idx:%x, taken:%d, target:%x.\n",
        ftq_idx, start_pc, cfi_idx, pred_taken, target);

    // gen squash info
    preDecSquash.squash = false;
    preDecSquash.branchTaken = false;
    preDecSquash.ftqIdx = ftq_idx;

    std::map<uint32_t, DynInstPtr> idx_insts;
    std::map<uint32_t, DynInstPtr> idx_insts_invalid;
    bool invalid_inst_flag = false;
    unsigned end_idx = 0;
    for (auto& inst : insts) {
        Addr inst_pc = inst->pcState().instAddr();
        bool is_ret = (inst->isReturn() && inst->isIndirectCtrl());
        Addr inst_target = inst->isDirectCtrl() ?
            inst->branchTarget()->instAddr() : is_ret ?
            branchPred->getRetAddr(ftq_idx) :
            readNpc(inst->pcState());
        uint32_t inst_idx = (inst_pc - start_pc)/2;

        if (invalid_inst_flag ||
            (!pred_taken && (inst_pc >= end_pc)) ||
            (pred_taken && (cfi_idx < inst_idx))) {
            idx_insts_invalid[inst_idx] = inst;
            invalid_insts.push_back(inst);
            continue;
        } else {
            idx_insts[inst_idx] = inst;
            valid_insts.push_back(inst);
            end_idx = inst_idx;
            if (inst->notAnInst()) {
                fault_idx = inst_idx;
                invalid_inst_flag = true;
                break;
            }
        }

        // fault check
        bool jal_fault = inst->isDirectCtrl() && inst->isUncondCtrl()
                       && (!pred_taken || (cfi_idx > inst_idx));
        bool ret_fault = is_ret && (!pred_taken || (cfi_idx > inst_idx)
                                    || (target != inst_target));
        bool target_fault = inst->isDirectCtrl() && pred_taken &&
                            (cfi_idx == inst_idx) &&
                            (target != inst_target);
        if ((jal_fault || ret_fault || target_fault) &&
           !preDecSquash.squash) {
            preDecSquash.squash = true;
            preDecSquash.seqNum = inst->seqNum;
            preDecSquash.branchAddr = inst_pc;
            preDecSquash.target = inst_target;
            preDecSquash.branchTaken = true;
            preDecSquash.squashInst = inst;
            std::unique_ptr<PCStateBase> target_pc(newPcState(inst_target));
            inst->setPredTarg(*target_pc);
            inst->setPredTaken(true);
            invalid_inst_flag = true;
        } else if (inst->isControl() && pred_taken && (cfi_idx == inst_idx)) {
            std::unique_ptr<PCStateBase> target_pc(newPcState(target));
            inst->setPredTarg(*target_pc);
            inst->setPredTaken(true);
            preDecSquash.squash = false;
            preDecSquash.squashInst = inst;
            invalid_inst_flag = true;
        }
        // else if (inst->isUncondCtrl()) {
        //    invalid_inst_flag = true;
        //}

        if (inst->isControl()) {
            ++fetchStats.branches;
        }
    }
    DPRINTF(Fetch, "insts size is:%d\n", insts.size());
    DPRINTF(Fetch, "invalid insts size is:%d\n", invalid_insts.size());
    DPRINTF(Fetch, "valid_insts size is:%d\n", valid_insts.size());
    if ((cfi_idx < fault_idx) && !preDecSquash.squash && pred_taken) {
        if (idx_insts.find(cfi_idx) == idx_insts.end()) { // invalidTaken
            preDecSquash.squash = true;
            preDecSquash.branchTaken = false;
            preDecSquash.target = start_pc + cfi_idx*2 + 2;
            if (cfi_idx != 0) {
                auto& inst = idx_insts.at(cfi_idx-1);
                preDecSquash.branchAddr = inst->pcState().instAddr();
                preDecSquash.seqNum = inst->seqNum;
                preDecSquash.squashInst = inst;
            }
            ftb_false_hit++;
            DPRINTF(Fetch, "invalidTaken fault, ftb_false_hit count = %d.\n",
                ftb_false_hit);
        } else if (!idx_insts.at(cfi_idx)->isControl()) { // notCFITaken
            auto& inst = idx_insts.at(cfi_idx);
            preDecSquash.squash = true;
            preDecSquash.branchTaken = false;
            preDecSquash.branchAddr = inst->pcState().instAddr();
            preDecSquash.seqNum = inst->seqNum;
            preDecSquash.target = inst->readPredTarg().instAddr();
            preDecSquash.squashInst = inst;
            ftb_false_hit++;
            DPRINTF(Fetch, "notCFITaken fault, ftb_false_hit count = %d.\n",
                ftb_false_hit);
        }
    }

    // gen pd info
    pdInfo.valid = true;
    pdInfo.ftqIdx = ftq_idx;

    // check btb_hit
    pdInfo.false_hit = false;
    BTBBranchInfo btbInfo = branchPred->get_ftb_for_precheck(ftq_idx);
    if (btbInfo.btb_hit) {
        if (preDecSquash.squash) {
            pdInfo.false_hit = true;
            DPRINTF(Fetch, "%d.\n", __LINE__);
        } else {
            auto& btb_entry = btbInfo.hitBtbEntry;
            auto& br_slot = btb_entry.slot[0];
            if (br_slot.valid && (br_slot.offset <= end_idx)) {
                if (idx_insts.find(br_slot.offset) != idx_insts.end()) {
                    auto inst = idx_insts.at(br_slot.offset);
                    pdInfo.false_hit = (!inst->isCondCtrl());
                } else {
                    pdInfo.false_hit = true;
                    DPRINTF(Fetch, "%d.\n", __LINE__);
                }
            }
            auto& tail_slot = btb_entry.slot[1];
            if (tail_slot.valid && (tail_slot.offset <= end_idx)) {
                if (idx_insts.find(tail_slot.offset) != idx_insts.end()) {
                    auto inst = idx_insts.at(tail_slot.offset);
                    bool is_ret = inst->isReturn() && inst->isIndirectCtrl();
                    if (tail_slot.sharing) {  // sharing == 1
                        pdInfo.false_hit = !(inst->isCondCtrl());
                    } else if (inst->isUncondCtrl()) {  // sharing == 0
                                                        //  and uncond branch
                        if ((btb_entry.isCall && inst->isCall())
                        || (btb_entry.isRet && is_ret)
                        || (btb_entry.isJalr && inst->isIndirectCtrl())
                        || inst->isDirectCtrl()) {
                            pdInfo.false_hit = false;
                        } else {
                            pdInfo.false_hit = true;
                            DPRINTF(Fetch, "%d.\n", __LINE__);
                        }
                    } else {
                        pdInfo.false_hit = true;
                        DPRINTF(Fetch, "%d.\n", __LINE__);
                    }
                } else {
                    pdInfo.false_hit = true;
                    DPRINTF(Fetch, "%d.\n", __LINE__);
                }
            }
        }
    }

    // check idx
    pdInfo.hasJmp = false;
    for (auto ii=0; ii<gem5::o3::MaxInstrsPerBlock; ii++) {
        if ((idx_insts.find(ii) == idx_insts.end()) ||
            (ii > fault_idx)) {
            pdInfo.validInstr[ii] = false;
            pdInfo.brMask[ii] = false;
            pdInfo.rvcMask[ii] = false;
        } else {
            DynInstPtr inst = idx_insts.at(ii);
            pdInfo.validInstr[ii] = true;
            pdInfo.brMask[ii] = inst->isCondCtrl();
            pdInfo.rvcMask[ii] = inst->compressed();
            if (!pdInfo.hasJmp && inst->isUncondCtrl()) {
                pdInfo.hasJmp = true;
                pdInfo.isDirect = inst->isDirectCtrl();
                pdInfo.jmpOffset = ii;
                pdInfo.jmpTarget = !inst->isDirectCtrl() ?
                                   readNpc(inst->pcState()) :
                                   inst->branchTarget()->instAddr();
                pdInfo.jmpAttr = inst->isCall() ? (uint32_t)JmpType::CALL :
                    // exclude mret
                    (inst->isReturn() && inst->isIndirectCtrl()) ?
                                           (uint32_t)JmpType::RET :
                    inst->isDirectCtrl() ? (uint32_t)JmpType::JAL :
                    (uint32_t)JmpType::JALR; // inst->isIndirectCtrl()
                DPRINTF(Fetch, "set hasJmp, offset %d, Tgt %x, Attr %d.\n",
                    pdInfo.jmpOffset, pdInfo.jmpTarget,
                    (uint32_t)pdInfo.jmpAttr);
            }
        }
    }

    // for (auto& idx_inst : idx_insts_invalid) {
    //     auto idx = idx_inst.first;
    //     auto inst = idx_inst.second;
    //     if (!pdInfo.hasJmp && inst->isUncondCtrl() && (idx < fault_idx)) {
    //         pdInfo.hasJmp = true;
    //         pdInfo.isDirect = inst->isDirectCtrl();
    //         pdInfo.jmpOffset = idx;
    //         pdInfo.jmpTarget = !inst->isDirectCtrl() ? 0 :
    //                            inst->branchTarget()->instAddr();
    //         bool is_ret = inst->isReturn() && inst->isIndirectCtrl();
    //         pdInfo.jmpAttr = inst->isCall() ? (uint32_t)JmpType::CALL :
    //             is_ret ? (uint32_t)JmpType::RET :
    //             inst->isDirectCtrl() ? (uint32_t)JmpType::JAL :
    //             (uint32_t)JmpType::JALR; // inst->isIndirectCtrl()
    //     }
    // }
}
void
FetchStage::predecodeSquash(std::vector<DynInstPtr>& insts)
{
    if (!preDecSquash.squash || !preDecSquash.branchTaken) {
        return;
    }
    // rm invalid insts
    uint32_t seq_num = preDecSquash.seqNum;
    auto it=insts.begin();
    while (it!=insts.end()) {
        auto inst = *it;
        if (inst->seqNum > seq_num) {
            it = insts.erase(it);
        } else {
            it++;
        }
    }

}


void FetchStage::advance(void)
{
    if (!f0BlockFlag) {
        f0Queue.advance();
    }
    if (!f1BlockFlag) {
        f1Queue.advance();
    }
    if (!f2BlockFlag) {
        f2Queue.advance();
        decodeInstQueue.advance();
    }
    toIfuQueue.advance();
    toBpuQueue.advance();
    branchPred->advance();
}

void
FetchStage::sendInstToNextStage(TimeBuffer<FetchStruct>::wire& fromQ,
                           TimeBuffer<FetchStruct>::wire& toQ)
{
    unsigned q_size = fromQ->size;
    // copy to next buffer
    for (unsigned ii=0; ii<q_size; ii++)
    {
        const auto& inst = fromQ->insts[ii];
        if (!inst->isSquashed()) {
            toQ->insts[toQ->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction"
                    " to next stage\n",
                    0, inst->seqNum);
        }
    }
    // clear from buffer
    fromQ->size = 0;
}
void
FetchStage::setSquash(TimeBuffer<FetchStruct>::wire& instQ)
{
    for (unsigned ii=0; ii<instQ->size; ii++) {
        instQ->insts[ii]->setSquashed();
    }
    instQ->size = 0;
}
void
FetchStage::setSquash(TimeBuffer<std::vector<RequestPtr>>::wire& p_reqs)
{
    for (auto& req : *p_reqs) {
        setReqStatus(req, FetchReqStatus::Squashed);
        DPRINTF(Fetch, "[tid:%i] Squash req:%x \n",
                        0, req.get());
    }
    clear(p_reqs);
}
void
FetchStage::setSquash(TimeBuffer<std::vector<DynInstPtr>>::wire& p_insts)
{
    for (auto& inst : *p_insts) {
        DPRINTF(Fetch, "[tid:%i] Squashing instruction, "
                "[sn:%lli] PC %s\n",
                inst->threadNumber,
                inst->seqNum,
                inst->pcState());
        if (inst->isSquashed()) {
            continue;
        }
        cpu->squashAndremoveInst(inst);
    }
    p_insts->clear();
}
void
FetchStage::setSquash(TimeBuffer<BpuToFetchStruct>::wire& p_ftq_info)
{
    DPRINTF(Fetch, "squash ftqInfo, startAddr:%x\n",
            p_ftq_info->instBlkFetchInfo.startAddr);
    p_ftq_info->instBlkFetchInfo.valid = false;
}

void
FetchStage::clear(TimeBuffer<std::vector<RequestPtr>>::wire& p_reqs)
{
    p_reqs->clear();
}

void
FetchStage::clear(RequestPtr req,
            std::unordered_map<RequestPtr, uint8_t *>& req_buffers)
{
    if (req_buffers.find(req) != req_buffers.end()) {
        delete [] req_buffers.at(req);
        req_buffers.at(req) = nullptr;
    }
}
void
FetchStage::clear(std::unordered_map<RequestPtr, uint8_t *>& req_buffers)
{
    for (auto& req_buffer : req_buffers) {
        uint8_t* buffer = req_buffer.second;
        delete [] buffer;
        buffer = nullptr;
    }
    req_buffers.clear();
}

bool
FetchStage::isValid(TimeBuffer<std::vector<RequestPtr>>::wire& p_reqs)
{
    return (p_reqs->size() > 0);
}
bool
FetchStage::isValid(RequestPtr p_req)
{
    return ((fetchReqList.find(p_req) != fetchReqList.end())
        && !isStatus(p_req, FetchReqStatus::Squashed)
        && !isStatus(p_req, FetchReqStatus::Finish)
        && !isStatus(p_req, FetchReqStatus::NoGoodAddr));
}
bool
FetchStage::isValid(std::vector<RequestPtr>& mem_reqs)
{
    bool ret = !mem_reqs.empty();
    for (auto p_req : mem_reqs) {
        ret = ret && isValid(p_req);
    }
    return ret;
}
void
FetchStage::tick0()
{
    DPRINTF(Fetch, "Running F0 stage.\n");

    ThreadID tid = 0;
    // std::vector<RequestPtr> fetch_reqs;

    // read FTQ info
    BpuToFetchStruct& new_ftq_info = *fromBpu;
    FtqFetchInfo& fetch_info = new_ftq_info.instBlkFetchInfo;
    bool intra_squash = new_ftq_info.intra_squash;

    // proc bpu squash, before reading fetchInfo
    if (intra_squash) {
        //assert(new_ftq_info.intra_squashed_ftq_idxs.size()>0);
        if (new_ftq_info.intra_squashed_ftq_idxs.size()>0) {
            intraSquash(0, new_ftq_info.intra_squashed_ftq_idxs);
        }
    }

    // save Fetch info to skid buffer
    if (fetch_info.valid) {
        assert(ftqFetchInfoBuffer.size() < ftqInfoBufferMaxSize);
        DPRINTF(Fetch, "Get fetch ftq info, ftqIdx:%i, "
                "startAddr:%x, endAddr:%x, taken:%d \n",
                fetch_info.ftqIdx,
                fetch_info.startAddr,
                fetch_info.endAddr,
                fetch_info.predTaken);
        ftqFetchInfoBuffer.push_back(fetch_info);
    }

    // init
    f0BlockFlag = false;
    if (isBackPressure<std::vector<RequestPtr>>(f0Queue)) {
        DPRINTF(Fetch, "F0 back pressured.\n");
        f0BlockFlag = true;
        return;
    }

    // proc fetch
    if (ftqFetchInfoBuffer.empty()) {
        ++fetchStats.ftqStallCycles;
        DPRINTF(Fetch, "Wait FTQ info to fetch.\n");
    } else {
        // Fetch each of the actively fetching threads.
        for (ThreadID i = 0; i < numThreads; ++i) {
            if (fetchReq[i].size() > 0) {
                issuePipelinedIfetch[i] = false;
            } else {
                issuePipelinedIfetch[i] = true;
            }
        }

        // Issue the next I-cache request if possible.
        for (ThreadID i = 0; i < numThreads; ++i) {
            if (issuePipelinedIfetch[i]) {
                auto& ftq_info = ftqFetchInfoBuffer.front();
                auto start_addr = ftq_info.startAddr;
                fetchReq[i] = pipelineIcacheAccesses(i, start_addr, ftq_info);
            }
        }
    }

    // proc prefetch
    FtqPrefetchInfo& prefetch_info = new_ftq_info.instBlkPrefetchInfo;
    if (enInstPrefetch && prefetch_info.valid
        && (prefetchCount < prefetchMax)) {
        DPRINTF(Fetch, "Get prefetch ftq info, startAddr:%x \n",
                  prefetch_info.startAddr);
        auto& ftq_info = ftqFetchInfoBuffer.front();
        auto start_addr = prefetch_info.startAddr;
        Addr prefetch_block = fetchBufferAlignPC(start_addr);
        bool need_prefetch = true;
        for (auto& req : fetchReq[tid]) {
            Addr fetch_block = fetchBufferAlignPC(getReqPc(req).instAddr());
        DPRINTF(Fetch, "block fetch:%x, prefetch:%x \n",
            fetch_block, prefetch_block);
            if (fetch_block == prefetch_block) {
                need_prefetch = false;
                break;
            }
        }
        if (prefetchReq[tid].size() > 0) {
            if (hasStatus(prefetchReq[tid], FetchReqStatus::ItlbWait)) {
                need_prefetch = false;
            } else {
                prefetchReq[tid].clear();
            }
        }
        if (need_prefetch) {
            prefetchReq[tid] = pipelineIcacheAccesses(tid, start_addr,
                                                      ftq_info, true);
            if (prefetchReq[tid].size() > 0) {
                prefetchCount++;
                DPRINTF(Fetch, "[tid:%i] prefetch new req: %x\n",
                    tid, prefetchReq[tid].at(0).get());
            }
        }
    }

    // send req to F1
    std::vector<FetchReqStatus> f0_finish_stat = {
        FetchReqStatus::ItlbFault,
        FetchReqStatus::IcacheWaitResponse,
        FetchReqStatus::IcacheAccessComplete};
    if (isStatus(fetchReq[tid], f0_finish_stat)) {
        DPRINTF(Fetch, "new req: %x and send to F1\n",
                fetchReq[tid].at(0).get());
        *toF1 = fetchReq[tid];
        fetchReq[tid].clear();
        ftqFetchInfoBuffer.pop_front();
        DPRINTF(Fetch, "fetch info pop, size: %d\n",
            ftqFetchInfoBuffer.size());
    } else if ((fetchReq[tid].size() > 0) && !isValid(fetchReq[0])) {
        DPRINTF(Fetch, "rm invalid new req: %x\n",
                fetchReq[tid].at(0).get());
        eraseReq(fetchReq[tid], FetchReqStatus::Squashed);
        fetchReq[tid].clear();
        ftqFetchInfoBuffer.pop_front();
    } else if (!ftqFetchInfoBuffer.empty()) {   // need fetch or not
        if (fetchReq[tid].size() > 0) { // wait tlb or retry
            DPRINTF(Fetch, "F0 block, req: %x\n", fetchReq[tid].at(0).get());
        } else {  // out of MSHR
            DPRINTF(Fetch, "F0 block\n");
        }
        f0BlockFlag = true;

        if (hasStatus(fetchReq[tid], FetchReqStatus::ItlbWait)) {
            ++fetchStats.tlbCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
            "finish!\n", tid);
        } else if (hasStatus(fetchReq[tid], FetchReqStatus::IcacheWaitRetry)) {
            ++fetchStats.icacheWaitRetryStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                    tid);
        }
    }
}

void
FetchStage::sendInstToDecode(void)
{
    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            random_mt.random<uint8_t>(0, activeThreads->size() - 1));

    while (available_insts != 0 && insts_to_decode < decodeWidth) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to Decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());
            bool taken = inst->readPredTaken();
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] taken:%d\n",
                tid, inst->seqNum, taken);
            if (taken) {
                DPRINTF(Fetch, "[tid:%i] [sn:%llu] target:%x \n",
                    tid, inst->seqNum, inst->readPredTarg().instAddr());
            }


            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

}


bool
FetchStage::checkSignalsAndUpdate(void)
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;
    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
    }

    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            interruptPending = false;
        }
    }
    return status_change;
}
bool
FetchStage::checkSignalsAndUpdate(ThreadID tid)
{
    // Update the per thread stall statuses.
    if (fromDecode->decodeBlock[tid]) {
        stalls[tid].decode = true;
    }

    if (fromDecode->decodeUnblock[tid]) {
        assert(stalls[tid].decode);
        assert(!fromDecode->decodeBlock[tid]);
        stalls[tid].decode = false;
    }

    // Check squash signals from commit.

    // check delay squash which matched ftq
    auto& commit_info = fromCommitToIfu->commitInfo[tid];
    if (commit_info.squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",tid);

        // In any case, squash.
        doSquash(*(commit_info.pc),
               commit_info.squashInst,
               tid);
        // clear fetch queue
        fetchQueueSquash();
        return true;
    }

    // Check squash signals from decode.
    // check delay squash which matched ftq
    auto& decode_info = fromDecodeToIfu->decodeInfo[tid];
    if (decode_info.squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                decode_info.squashInst->pcState());
            // Squash unless we're already squashing
            doSquash(*(decode_info.nextPC),
                     decode_info.squashInst,
                     tid);
            return true;
        }
    }

    // Check squash signals from IFU precheck.
    PreDecSquash& pre_dec_squash = fromIfu->preDecSquash;
    if (pre_dec_squash.squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from precheck.\n",0);

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from precheck with PC = %s,"
                    "ftqIdx = %d\n",
                    pre_dec_squash.squashInst->pcState(),
                    pre_dec_squash.ftqIdx);
            // Squash unless we're already squashing
            std::unique_ptr<PCStateBase> target_pc(
                newPcState(pre_dec_squash.target));
            doSquash(*target_pc,
                     pre_dec_squash.squashInst,
                     0);
            return true;
        }
    }

    if (checkStall(tid)/* &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != QuiescePending*/) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        fetchStatus[tid] = Blocked;

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        fetchStatus[tid] = Running;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

DynInstPtr
FetchStage::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);
    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    return instruction;
}

bool
FetchStage::predecode(std::vector<RequestPtr>& reqs, unsigned ftq_idx)
{
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        return false;
    }
    // fetchQueue full or not
    bool fetch_q_full = ((fetchQueue[tid].size() + fetchWidth) >
                         fetchQueueSize);
    if (fetch_q_full) {
      return false;
    }
    // check cache line data valid or not
    fetchBufferValid[tid] = (fetchBuffer[tid].find(reqs.at(0))
                             != fetchBuffer[tid].end());
    for (auto it=reqs.begin()+1; it!=reqs.end(); it++) {
        bool valid = (fetchBuffer[tid].find(*it) != fetchBuffer[tid].end());
        fetchBufferValid[tid] = fetchBufferValid[tid] && valid;
    }
    if (!fetchBufferValid[tid]) {
      return false;
    }

    // fetch valid codes
    auto& fetch_info = getReqFetchInfo(reqs.at(0));
    Addr start_addr = fetch_info.startAddr;
    Addr end_addr = start_addr+32;
    DPRINTF(Fetch, "precess ftq info req data, "
                "ftqIdx:%i, startAddr:%x, endAddr:%x \n",
                fetch_info.ftqIdx,
                start_addr, end_addr);
    // One compress instruction takes up 2 bytes
    uint32_t fetch_byte = fetchWidth / 2 * instSize;
    // copy data need to get more 2 bytes
    uint32_t copy_len = fetch_byte + 2;
    // get more unaligned bytes
    const Addr offset_mask = instSize - 1;
    copy_len += (instSize - (start_addr & offset_mask)) & offset_mask;
    Addr copy_start = getReqPc(reqs.at(0)).instAddr();

    std::vector<uint8_t> fetch_buffer(copy_len+4, 0);
    uint32_t offset = 0;
    for (auto& req : reqs) {
        if (!isStatus(req, FetchReqStatus::IcacheAccessComplete)) {
            break;
        }
        auto& cache_buffer = fetchBuffer[tid].at(req);
        uint32_t start_offset = getReqPc(req).instAddr() & fetchBufferMask;
        uint32_t len = (fetchBufferSize - start_offset) < copy_len ?
                       (fetchBufferSize - start_offset) : copy_len ;
        memcpy(fetch_buffer.data() + offset, cache_buffer + start_offset, len);
        DPRINTF(Fetch, "Copy data to decode, req:%x, addr:%x, size:%d \n",
                req.get(), getReqPc(req).instAddr(), len);
        fetchBufferValid[tid] = true;
        copy_len -= len;
        offset += len;
    }

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.
    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);

    // Need to halt fetch if quiesce instruction detected
    //bool quiesce = false;

    // Reset the number of the instruction we've fetched.
    numInst = 0;
    auto *dec_ptr = decoder[tid];
    Addr pcOffset = (start_addr == preFtqInfoEndAddr[tid]) ? fetchOffset[tid]
                                                           : 0;
    Addr fetchAddr = start_addr + pcOffset;//getReqPc(reqs.at(0)).instAddr();
    Addr end_fetch_addr = (copy_len == 0) ?
                          (start_addr + fetch_byte + 2) :
                          (start_addr + offset);
    unsigned blkOffset = (fetchAddr - copy_start) / instSize;
    assert(blkOffset < 2);
    std::unique_ptr<PCStateBase> this_pc(newPcState(fetchAddr));
    StaticInstPtr staticInst = NULL;
    StaticInstPtr curMacroop = macroop[tid];
    bool inRom = isRomMicroPC(this_pc->microPC());
    fetchBufferPC[tid] = getReqPc(reqs.at(0)).instAddr();
    const Addr pc_mask = dec_ptr->pcMask();
    end_addr = (copy_len == 0) ? end_addr : fetchAddr;

    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    while (fetchBufferValid[tid] &&
           (numInst < fetchWidth) &&
           (end_fetch_addr > this_pc->instAddr()) &&
           (end_addr > this_pc->instAddr())) {
        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        bool needMem = !inRom && !curMacroop && !dec_ptr->instReady();

        if (needMem) {
            DPRINTF(Fetch, "Supplying fetch from fetchBuffer\n");
            // data aligned to instSize
            memcpy(dec_ptr->moreBytesPtr(),
                   fetch_buffer.data() + blkOffset * instSize, instSize);
            dec_ptr->moreBytes(*this_pc, fetchAddr);

            if (dec_ptr->needMoreBytes()) {
                blkOffset++;
                fetchAddr += instSize;
                pcOffset += instSize;
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            bool compressed_flag = false;
            if (!(curMacroop || inRom)) {
                if (dec_ptr->instReady()) {
                    staticInst = dec_ptr->decode(*this_pc);

                    if (staticInst->isMacroop()) {
                        curMacroop = staticInst;
                    } else {
                        pcOffset = 0;
                    }
                    auto& pc_state = (*this_pc).as<RiscvISA::PCState>();
                    compressed_flag = pc_state.compressed();
                } else {
                    // We need more bytes for this instruction so blkOffset and
                    // pcOffset will be updated
                    break;
                }
            }
            // Whether we're moving to a new macroop because we're at the
            // end of the current one, or the branch predictor incorrectly
            // thinks we are...
            bool newMacro = false;
            if (curMacroop || inRom) {
                if (inRom) {
                    staticInst = dec_ptr->fetchRomMicroop(
                            this_pc->microPC(), curMacroop);
                } else {
                    staticInst = curMacroop->fetchMicroop(this_pc->microPC());
                }
                newMacro |= staticInst->isLastMicroop();
            }

            DynInstPtr instruction = buildInst(
                    tid, staticInst, curMacroop, *this_pc, *this_pc, true);
            instruction->ftqIdx = ftq_idx;
            instruction->compressed(compressed_flag);
            // update pc to next inst
            Addr this_addr = this_pc->instAddr();
            staticInst->advancePC(*this_pc);
            // default pred target to next pc
            instruction->setPredTarg(*this_pc);
            instruction->setPredTaken(false);

            DPRINTF(Fetch, "[tid:%i] Build instruction PC %s,"
                           "seq num %i, ftq idx %i.\n",
                            0, instruction->pcState(),
                            instruction->seqNum, instruction->ftqIdx);
            DPRINTF(Fetch, "[tid:%i] after advance PC %s\n", 0, *this_pc);
            // Write the instruction to the tmp queue for precheck
            fetchInstQueue.push_back(instruction);

            ppFetch->notify(instruction);
            numInst++;

            newMacro |= (this_addr != this_pc->instAddr());
            inRom = isRomMicroPC(this_pc->microPC());

            if (newMacro) {
                fetchAddr = this_pc->instAddr() & pc_mask;
                blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
                pcOffset = 0;
                curMacroop = NULL;
            }
#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
            }
#endif

            if (instruction->isQuiesce()) {
                DPRINTF(Fetch,
                        "Quiesce instruction encountered, halting fetch!\n");
                fetchStatus[tid] = QuiescePending;
                //quiesce = true;
                break;
            }
        } while (curMacroop   // macro inst and not the last op
            || (dec_ptr->instReady() && numInst < fetchWidth &&
                 end_fetch_addr > this_pc->instAddr()));

        // Re-evaluate whether the next instruction to fetch is in micro-op ROM
        // or not.
        inRom = isRomMicroPC(this_pc->microPC());
    }

    for (auto& req : reqs) {
        if (isStatus(req, FetchReqStatus::ItlbFault)) {
            const PCStateBase &fetch_pc = getReqPc(req);
            Addr fetch_addr = fetch_pc.instAddr();
            auto& fetch_info = getReqFetchInfo(req);
            Addr start_addr = fetch_info.startAddr;
            Addr inst_addr = (fetch_addr < start_addr) ?
                            start_addr : fetch_addr;
            std::unique_ptr<PCStateBase> inst_pc(newPcState(inst_addr));
            DPRINTF(Fetch, "[tid:%i] Translation faulted,"
                " building noop, PC %s.\n", tid, *inst_pc);
            // We will use a nop in ordier to carry the fault.
            DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
                    *inst_pc, *inst_pc, false);
            instruction->setNotAnInst();
            instruction->setPredTarg(*inst_pc);
            std::unique_ptr<PCStateBase> next_pc(inst_pc->clone());
            instruction->staticInst->advancePC(*next_pc);
            set(instruction->predPC, next_pc);

            instruction->fault = getReqFault(req);
            instruction->ftqIdx = ftq_idx;
            instruction->setPredTaken(false);
            fetchInstQueue.push_back(instruction);
            numInst++;
            break;
        }
    }

    macroop[tid] = curMacroop;
    assert(idxToOffset.find(ftq_idx) == idxToOffset.end());
    idxToOffset[ftq_idx] = std::pair<Addr, Addr>(fetchOffset[tid],
        preFtqInfoEndAddr[tid]);  // backup offset for idx
    DPRINTF(Fetch, "store ftqIdx:%d fetchOffset :%d\n",
        ftq_idx, fetchOffset[tid]);
    assert(this_pc->instAddr() >= end_addr);
    fetchOffset[tid] = (this_pc->instAddr() - end_addr) & offset_mask;
    preFtqInfoEndAddr[tid] = end_addr;
    decoder[tid]->reset();
    // clear icache data in buffer
    for (auto& req : reqs) {
        clear(req, fetchBuffer[tid]);
        fetchBuffer[tid].erase(req);
        DPRINTF(Fetch, " erase memReq:%x, fetchBuffer size:%d.\n",
                req.get(), fetchBuffer[tid].size());
    }

    if (fetchInstQueue.size() > 0) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch %d "
                "for this cycle.\n", tid, fetchInstQueue.size());
        return true;
    }

    return false;
}

void
FetchStage::recvReqRetry()
{
    if (!retryPktQ.empty()) {
        assert(cacheBlocked);
        assert(retryTid != InvalidThreadID);
        //assert(fetchStatus[retryTid] == IcacheWaitRetry);
        auto retry_pkt = retryPktQ.front();
        DPRINTF(Fetch, "recvReqRetry, req:%x, size:%d.\n",
                retry_pkt->req.get(), retryPktQ.size());

        assert(isStatus(retry_pkt->req, IcacheWaitRetry));
        if (icachePort.sendTimingReq(retry_pkt)) {
            retryPktQ.pop();
            //fetchStatus[retryTid] = IcacheWaitResponse;
            setReqStatus(retry_pkt->req, IcacheWaitResponse);
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify(retry_pkt->req);
            if (!retryPktQ.empty()) {
                retry_pkt = retryPktQ.front();
                if (icachePort.sendTimingReq(retry_pkt)) {
                    retryPktQ.pop();
                    setReqStatus(retry_pkt->req, IcacheWaitResponse);
                    ppFetchRequestSent->notify(retry_pkt->req);
                    retryTid = InvalidThreadID;
                    cacheBlocked = false;
                }
            } else {
                retryTid = InvalidThreadID;
                cacheBlocked = false;
            }
        }
    } else {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
    }
}

///////////////////////////////////////
//                                   //
//  SMT FETCH POLICY MAINTAINED HERE //
//                                   //
///////////////////////////////////////
ThreadID
FetchStage::getFetchingThread()
{
    if (numThreads > 1) {
        switch (fetchPolicy) {
          case SMTFetchPolicy::RoundRobin:
            return roundRobin();
          case SMTFetchPolicy::IQCount:
            return iqCount();
          case SMTFetchPolicy::LSQCount:
            return lsqCount();
          case SMTFetchPolicy::Branch:
            return branchCount();
          default:
            return InvalidThreadID;
        }
    } else {
        std::list<ThreadID>::iterator thread = activeThreads->begin();
        if (thread == activeThreads->end()) {
            return InvalidThreadID;
        }

        ThreadID tid = *thread;

        if (fetchStatus[tid] == Running ||
            //fetchStatus[tid] == IcacheAccessComplete ||
            fetchStatus[tid] == Idle) {
            return tid;
        } else {
            return InvalidThreadID;
        }
    }
}


ThreadID
FetchStage::roundRobin()
{
    std::list<ThreadID>::iterator pri_iter = priorityList.begin();
    std::list<ThreadID>::iterator end      = priorityList.end();

    ThreadID high_pri;

    while (pri_iter != end) {
        high_pri = *pri_iter;

        assert(high_pri <= numThreads);

        if (fetchStatus[high_pri] == Running ||
            //fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle) {

            priorityList.erase(pri_iter);
            priorityList.push_back(high_pri);

            return high_pri;
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
FetchStage::iqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned iqCount = fromIEW->iewInfo[tid].iqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(iqCount);
        threadMap[iqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            //fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();

    }

    return InvalidThreadID;
}

ThreadID
FetchStage::lsqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned ldstqCount = fromIEW->iewInfo[tid].ldstqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(ldstqCount);
        threadMap[ldstqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            //fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
FetchStage::branchCount()
{
    panic("Branch Count Fetch policy unimplemented\n");
    return InvalidThreadID;
}

std::vector<RequestPtr>
FetchStage::pipelineIcacheAccesses(ThreadID tid,
                                  Addr start_addr,
                                  const FtqFetchInfo& fetch_info,
                                  bool is_prefetch)
{
    DPRINTF(Fetch, "[tid:%i] IcacheAccesses PC %x\n",
        tid, start_addr);
    start_addr = start_addr & decoder[tid]->pcMask();
    uint32_t fetch_byte = fetchWidth / 2 * instSize;
    Addr end_addr = start_addr + fetch_byte + 2;
    Addr start_addr_block = fetchBufferAlignPC(start_addr);
    Addr end_addr_block = fetchBufferAlignPC(end_addr);
    std::vector<Addr> fetch_addrs;
    fetch_addrs.push_back(start_addr);
    if ((start_addr_block != end_addr_block) && !is_prefetch) {
        fetch_addrs.push_back(end_addr_block);
    }

    bool req_invalid = false;
    std::vector<RequestPtr> new_reqs;
    new_reqs.clear();
    for (auto addr : fetch_addrs) {
        pc[tid].reset(newPcState(addr));
        auto req = fetchCacheLine(addr, tid, *pc[tid], is_prefetch);
        // if (isStatus(req, IcacheWaitResponse)) {
            new_reqs.push_back(req);
        // } else {
        //     req_invalid = true;
        //     break;
        // }
        if (cacheBlocked) {
            req_invalid = true;
            break;
        }
    }

    //if (cacheBlocked) {
    if (req_invalid) {
        if (!is_prefetch) {
            while (!retryPktQ.empty())
            {
              auto retry_pkt = retryPktQ.front();
              delete retry_pkt;
              retry_pkt = NULL;
              retryPktQ.pop();
            }
            retryTid = InvalidThreadID;
            cacheBlocked = false;
        }
        eraseReq(new_reqs, FetchReqStatus::Squashed);
        new_reqs.clear();
    } else if ((new_reqs.size() > 0) && !is_prefetch) {
        setReqFetchInfo(new_reqs, fetch_info);
    }

    return new_reqs;
}

void
FetchStage::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (stalls[tid].drain) {
        ++fetchStats.pendingDrainCycles;
        DPRINTF(Fetch, "Fetch is waiting for a drain!\n");
    } else if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    // } else if (fetchStatus[tid] == IcacheWaitResponse) {
    //     ++fetchStats.icacheStallCycles;
    //     DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
    //             tid);
    // } else if (fetchStatus[tid] == ItlbWait) {
    //     ++fetchStats.tlbCycles;
    //     DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
    //             "finish!\n", tid);
    // } else if (fetchStatus[tid] == TrapPending) {
    //     ++fetchStats.pendingTrapStallCycles;
    //     DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
    //             tid);
    } else if (fetchStatus[tid] == QuiescePending) {
        ++fetchStats.pendingQuiesceStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending quiesce "
                "instruction!\n", tid);
    // } else if (fetchStatus[tid] == IcacheWaitRetry) {
    //     ++fetchStats.icacheWaitRetryStallCycles;
    //     DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
    //             tid);
    // } else if (fetchStatus[tid] == NoGoodAddr) {
    //         DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable"
    //                        " address\n", tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

bool
FetchStage::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));
    fetch->processCacheCompletion(pkt);

    return true;
}

void
FetchStage::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

bool
FetchStage::isStatus(const RequestPtr &mem_req, FetchReqStatus stat)
{
    if (fetchReqList.find(mem_req) != fetchReqList.end()) {
        return (fetchReqList.at(mem_req)->status == stat);
    }
    return false;
}
bool
FetchStage::isStatus(const std::vector<RequestPtr> &reqs, FetchReqStatus stat)
{
    if (reqs.size() == 0) {
        return false;
    }
    bool ret = true;
    for (auto& req : reqs) {
        ret = ret && isStatus(req, stat);
    }
    return ret;
}
bool
FetchStage::isStatus(const std::vector<RequestPtr> &reqs,
                const std::vector<FetchReqStatus> stats)
{
    if (reqs.size() == 0) {
        return false;
    }
    bool ret = true;
    bool has_stat = false;
    for (auto& req : reqs) {
        if (fetchReqList.find(req) != fetchReqList.end()) {
            auto stat = fetchReqList.at(req)->status;
            has_stat = (std::find(stats.begin(), stats.end(), stat)
                      != stats.end());
        } else {
            has_stat = false;
        }
        ret = ret && has_stat;
    }
    return ret;
}

bool
FetchStage::hasStatus(const std::vector<RequestPtr> &reqs, FetchReqStatus stat)
{
    bool ret = false;
    for (auto& req : reqs) {
        ret = ret || isStatus(req, stat);
    }
    return ret;
}
bool
FetchStage::eraseReq(std::vector<RequestPtr> &reqs, FetchReqStatus stat)
{
    bool ret = true;
    for (auto& mem_req : reqs) {
        ret = ret && eraseReq(mem_req, stat);
        mem_req = nullptr;
    }
    reqs.clear();
    return ret;
}

bool
FetchStage::eraseReq(const RequestPtr &mem_req, FetchReqStatus stat)
{
    if (fetchReqList.find(mem_req) != fetchReqList.end()) {
        setReqStatus(mem_req, stat);
        auto ftq_idx = fetchReqList.at(mem_req)->fetchInfo.ftqIdx;
        if (ftqIdxReqs.find(ftq_idx) != ftqIdxReqs.end()) {
            ftqIdxReqs.erase(ftq_idx);
        }
        fetchReqList.erase(mem_req);
        DPRINTF(Fetch, " eraseReq, memReq:%x, fetchReqList size:%d.\n",
                mem_req.get(), fetchReqList.size());
        return true;
    }
    return false;
}
void
FetchStage::pushReqToList(const RequestPtr &mem_req,
                          const PCStateBase& this_pc)
{
    fetchReqList[mem_req] = std::make_unique<ReqInfo>(this_pc,
                            FetchReqStatus::ItlbWait);
    //assert(fetchReqList.size() < 18);
    DPRINTF(Fetch, " push to list, memReq:%x, pc:%x, size:%d.\n",
            mem_req.get(), this_pc.instAddr(), fetchReqList.size());
}
void
FetchStage::updateReqList(void)
{
    auto it=fetchReqList.begin();
    while (it != fetchReqList.end()) {
        auto& mem_req = it->first;
        auto& status = it->second->status;
        DPRINTF(Fetch, " updateReqList:%x.\n", mem_req.get());
        if ((status == FetchReqStatus::Finish) ||
            (status == FetchReqStatus::Squashed) ||
            (status == FetchReqStatus::NoGoodAddr)) {
            auto ftq_idx = it->second->fetchInfo.ftqIdx;
            if (ftqIdxReqs.find(ftq_idx) != ftqIdxReqs.end()) {
                ftqIdxReqs.erase(ftq_idx);
            }
            it = fetchReqList.erase(it);
            DPRINTF(Fetch, " erase memReq:%x, fetchReqList size:%d.\n",
                    mem_req.get(), fetchReqList.size());
        } else {
            it++;
        }
    }
}
void
FetchStage::updateIcacheDataMap(void)
{
    for (auto tid=0; tid<MaxThreads; tid++) {
        auto it=fetchBuffer[tid].begin();
        while (it != fetchBuffer[tid].end()) {
            auto& mem_req = it->first;
            DPRINTF(Fetch, " updateIcacheDataMap:%x.\n", mem_req.get());
            if (isStatus(mem_req, FetchReqStatus::Finish) ||
                isStatus(mem_req, FetchReqStatus::Squashed) ||
                isStatus(mem_req, FetchReqStatus::NoGoodAddr)) {
                clear(mem_req, fetchBuffer[tid]);
                it = fetchBuffer[tid].erase(it);
                DPRINTF(Fetch, " erase memReq:%x, fetchBuffer size:%d.\n",
                        mem_req.get(), fetchBuffer[tid].size());
            } else {
                it++;
            }
        }
    }
}
void
FetchStage::setReqStatus(const RequestPtr &mem_req, FetchReqStatus stat)
{
    if (fetchReqList.find(mem_req) != fetchReqList.end()) {
        fetchReqList.at(mem_req)->setStatus(stat);
        DPRINTF(Fetch, " setReqStatus:%d, memReq:%x.\n",
                (uint32_t)stat, mem_req.get());
    }
}
void
FetchStage::setReqStatus(const std::vector<RequestPtr> &reqs,
                         FetchReqStatus stat)
{
    for (auto& req : reqs) {
        setReqStatus(req, stat);
    }
}
void
FetchStage::setReqFault(const RequestPtr &mem_req, Fault fault)
{
    if (fetchReqList.find(mem_req) != fetchReqList.end()) {
        fetchReqList.at(mem_req)->setFault(fault);
        DPRINTF(Fetch, " setReqFault:%s, memReq:%x.\n",
                fault, mem_req.get());
    }
}
void
FetchStage::setReqFetchInfo(const RequestPtr &mem_req,
                     const FtqFetchInfo& fetch_info)
{
    if (fetchReqList.find(mem_req) != fetchReqList.end()) {
        fetchReqList.at(mem_req)->setFtqInfo(fetch_info);
        DPRINTF(Fetch, " setReqFetchInfo ftqIdx:%d, memReq:%x\n",
                fetchReqList.at(mem_req)->fetchInfo.ftqIdx,
                mem_req.get());
    }
}
void
FetchStage::setReqFetchInfo(const std::vector<RequestPtr> &reqs,
                     const FtqFetchInfo& fetch_info)
{
    DPRINTF(Fetch, " setReqFetchInfo push ftqIdxReqs:%d, Req:%x\n",
        fetch_info.ftqIdx, reqs.at(0));
    ftqIdxReqs[fetch_info.ftqIdx] = reqs;
    for (auto& req : reqs) {
        setReqFetchInfo(req, fetch_info);
    }
}
PCStateBase&
FetchStage::getReqPc(const RequestPtr &mem_req)
{
    assert(fetchReqList.find(mem_req) != fetchReqList.end());
    return *(fetchReqList.at(mem_req)->pc);
}

Fault&
FetchStage::getReqFault(const RequestPtr &mem_req)
{
    assert(fetchReqList.find(mem_req) != fetchReqList.end());
    return (fetchReqList.at(mem_req)->fault);
}

FetchStage::FtqFetchInfo&
FetchStage::getReqFetchInfo(const RequestPtr &mem_req)
{
    assert(fetchReqList.find(mem_req) != fetchReqList.end());
    return (fetchReqList.at(mem_req)->fetchInfo);
}
bool
FetchStage::isEmpty(RequestPtr& req)
{
    return (req.get() == nullptr);
}
bool
FetchStage::isEmpty(std::vector<RequestPtr>& reqs)
{
    return (reqs.size() == 0);
}
bool
FetchStage::isEmpty(FetchStruct& insts)
{
    return (insts.size == 0);
}
bool
FetchStage::isEmpty(std::vector<DynInstPtr>& insts)
{
    return (insts.size() == 0);
}

PCStateBase*
FetchStage::newPcState(Addr pc)
{
    return cpu->newPCState(pc);
}

Addr
FetchStage::readNpc(const PCStateBase& pc)
{
    return (pc.as<GenericISA::PCStateWithNext>()).npc();
}

} // namespace o3
} // namespace gem5

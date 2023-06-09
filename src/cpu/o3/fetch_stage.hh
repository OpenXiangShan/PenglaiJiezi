/*
 * Copyright (c) 2010-2012, 2014 ARM Limited
 * All rights reserved
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

#ifndef __CPU_O3_FETCH_STAGE_HH__
#define __CPU_O3_FETCH_STAGE_HH__

#include <queue>

#include "arch/generic/decoder.hh"
#include "arch/generic/mmu.hh"
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pc_event.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/dppred_blk/DecoupledBPU.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/timebuf.hh"
#include "cpu/translation.hh"
#include "enums/SMTFetchPolicy.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sim/eventq.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{
using namespace gem5::branch_prediction;

class CPU;

/**
 * Fetch class handles both single threaded and SMT fetch. Its
 * width is specified by the parameters; each cycle it tries to fetch
 * that many instructions. It supports using a branch predictor to
 * predict direction and targets.
 * It supports the idling functionality of the CPU by indicating to
 * the CPU when it is active and inactive.
 */
class FetchStage
{
  public:
    /**
     * IcachePort class for instruction fetch.
     */
    class IcachePort : public RequestPort
    {
      protected:
        /** Pointer to fetch. */
        FetchStage *fetch;

      public:
        /** Default constructor. */
        IcachePort(FetchStage *_fetch, CPU *_cpu);

      protected:

        /** Timing version of receive.  Handles setting fetch to the
         * proper status to start fetching. */
        virtual bool recvTimingResp(PacketPtr pkt);

        /** Handles doing a retry of a failed fetch. */
        virtual void recvReqRetry();
    };

    class FetchTranslation : public BaseMMU::Translation
    {
      protected:
        FetchStage *fetch;

      public:
        FetchTranslation(FetchStage *_fetch) : fetch(_fetch) {}

        void markDelayed() {}

        void
        finish(const Fault &fault, const RequestPtr &req,
            gem5::ThreadContext *tc, BaseMMU::Mode mode)
        {
            assert(mode == BaseMMU::Execute);
            fetch->finishTranslation(fault, req);
            delete this;
        }
    };

  private:
    /* Event to delay delivery of a fetch translation result in case of
     * a fault and the nop to carry the fault cannot be generated
     * immediately */
    class FinishTranslationEvent : public Event
    {
      private:
        FetchStage *fetch;
        Fault fault;
        RequestPtr req;

      public:
        FinishTranslationEvent(FetchStage *_fetch)
            : fetch(_fetch), req(nullptr)
        {}

        void setFault(Fault _fault) { fault = _fault; }
        void setReq(const RequestPtr &_req) { req = _req; }

        /** Process the delayed finish translation */
        void
        process()
        {
            assert(fetch->numInst < fetch->fetchWidth);
            fetch->finishTranslation(fault, req);
        }

        const char *
        description() const
        {
            return "CPU FetchFinishTranslation";
        }
      };

  public:
    /** Overall fetch status. Used to determine if the CPU can
     * deschedule itsef due to a lack of activity.
     */
    enum FetchStatus
    {
        Active,
        Inactive
    };

    /** Individual thread status. */
    enum ThreadStatus
    {
        Running,
        Idle,
        Squashing,
        Blocked,
        Fetching,
        TrapPending,
        QuiescePending
    };

    enum FetchReqStatus
    {
        ItlbWait,
        ItlbFault,
        IcacheWaitRetry,
        IcacheWaitResponse,
        IcacheAccessComplete,
        NoGoodAddr,
        Squashed,
        Finish
    };

    using FtqFetchInfo = BpuToFetchStruct::InstBlkFetchInfo;
    using FtqPrefetchInfo = BpuToFetchStruct::InstBlkPrefetchInfo;
    class ReqInfo
    {
      public:
        bool isPrefetch = false;
        std::unique_ptr<PCStateBase> pc;
        FetchReqStatus status;
        Fault fault;
        FtqFetchInfo fetchInfo;
      public:
        ReqInfo(const PCStateBase& this_pc, FetchReqStatus stat)
        : pc(this_pc.clone())
        , status(stat)
        , fault(NoFault)
        {
            fetchInfo.ftqIdx = 0xffff; // init with invalid idx
        }
        void setFault(Fault _fault) { fault = _fault; }
        void setStatus(FetchReqStatus _stat) { status = _stat; }
        void setFtqInfo(const FtqFetchInfo& info) { fetchInfo = info; }
        void setPrefetch(bool flag) { isPrefetch = flag; }
    };
    typedef std::unique_ptr<ReqInfo> ReqInfoPtr;

  private:
    /** Fetch status. */
    FetchStatus _status;

    /** Per-thread status. */
    ThreadStatus fetchStatus[MaxThreads];

    /** Fetch policy. */
    SMTFetchPolicy fetchPolicy;

    /** List that has the threads organized by priority. */
    std::list<ThreadID> priorityList;

    /** Probe points. */
    ProbePointArg<DynInstPtr> *ppFetch;
    /** To probe when a fetch request is successfully sent. */
    ProbePointArg<RequestPtr> *ppFetchRequestSent;

    /** Enable inst prefetch **/
    bool enInstPrefetch;

    /** prefetch cmd count **/
    const unsigned prefetchMax = 4;
    unsigned prefetchCount;

  public:
    /** Fetch constructor. */
    FetchStage(CPU *_cpu, const BaseO3CPUParams &params);

    /** Returns the name of fetch. */
    std::string name() const;


    /** Registers probes. */
    void regProbePoints();

    /** Sets the main backwards communication time buffer pointer. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Sets pointer to time buffer used to communicate to the next stage. */
    void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr);

    /** Initialize stage. */
    void startupStage();

    /** Clear all thread-specific states*/
    void clearStates(ThreadID tid);

    /** Handles retrying the fetch access. */
    void recvReqRetry();

    /** Processes cache completion event. */
    void processCacheCompletion(PacketPtr pkt);

    /** Resume after a drain. */
    void drainResume();

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /** Takes over from another CPU's thread. */
    void takeOverFrom();

    /**
     * Stall the fetch stage after reaching a safe drain point.
     *
     * The CPU uses this method to stop fetching instructions from a
     * thread that has been drained. The drain stall is different from
     * all other stalls in that it is signaled instantly from the
     * commit stage (without the normal communication delay) when it
     * has reached a safe point to drain from.
     */
    void drainStall(ThreadID tid);

    /** Tells fetch to wake up from a quiesce instruction. */
    void wakeFromQuiesce();

    /** For priority-based fetch policies, need to keep update priorityList */
    void deactivateThread(ThreadID tid);
  private:
    /** Reset this pipeline stage */
    void resetStage();

    /** Changes the status of this stage to active, and indicates this
     * to the CPU.
     */
    void switchToActive();

    /** Changes the status of this stage to inactive, and indicates
     * this to the CPU.
     */
    void switchToInactive();

    /**
     * Looks up in the branch predictor to see if the next PC should be
     * either next PC+=MachInst or a branch target.
     * @param next_PC Next PC variable passed in by reference.  It is
     * expected to be set to the current PC; it will be updated with what
     * the next PC will be.
     * @param next_NPC Used for ISAs which use delay slots.
     * @return Whether or not a branch was predicted as taken.
     */
    bool lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &pc);

    /**
     * Fetches the cache line that contains the fetch PC.  Returns any
     * fault that happened.  Puts the data into the class variable
     * fetchBuffer, which may not hold the entire fetched cache line.
     * @param vaddr The memory address that is being fetched from.
     * @param ret_fault The fault reference that will be set to the result of
     * the icache access.
     * @param tid Thread id.
     * @param pc The actual PC of the current instruction.
     * @return Any fault that occured.
     */
    RequestPtr fetchCacheLine(Addr vaddr, ThreadID tid,
                              const PCStateBase &this_pc,
                              bool is_prefetch=false);
    void finishTranslation(const Fault &fault, const RequestPtr &mem_req);


    /** Check if an interrupt is pending and that we need to handle
     */
    bool checkInterrupt(Addr pc) { return interruptPending; }

    /** Squashes a specific thread and resets the PC. */
    void doSquash(const PCStateBase &new_pc, const DynInstPtr squashInst,
            ThreadID tid);

    /** Checks if a thread is stalled. */
    bool checkStall(ThreadID tid) const;

    /** Updates overall fetch stage status; to be called at the end of each
     * cycle. */
    FetchStatus updateFetchStatus();

  public:
    /** Ticks the fetch stage, processing all inputs signals and fetching
     * as many instructions as possible.
     */
    void tick();
    void tick0();
    void tick1();
    void tick2();
    void tick3();

    /** Checks all input signals and updates the status as necessary.
     *  @return: Returns if the status has changed due to input signals.
     */
    bool checkSignalsAndUpdate(ThreadID tid);
    bool checkSignalsAndUpdate(void);

    /** Does the actual fetching of instructions and passing them on to the
     * next stage.
     * @param status_change fetch() sets this variable if there was a status
     * change (ie switching to IcacheMissStall).
     */
    void fetch(bool &status_change);

    /** Align a PC to the start of a fetch buffer block. */
    Addr fetchBufferAlignPC(Addr addr)
    {
        return (addr & ~(fetchBufferMask));
    }

    /** The decoder. */
    InstDecoder *decoder[MaxThreads];

    RequestPort &getInstPort() { return icachePort; }

    void flushFetchBuffer();

  private:
    DynInstPtr buildInst(ThreadID tid, StaticInstPtr staticInst,
            StaticInstPtr curMacroop, const PCStateBase &this_pc,
            const PCStateBase &next_pc, bool trace);

    /** Returns the appropriate thread to fetch, given the fetch policy. */
    ThreadID getFetchingThread();

    /** Returns the appropriate thread to fetch using a round robin policy. */
    ThreadID roundRobin();

    /** Returns the appropriate thread to fetch using the IQ count policy. */
    ThreadID iqCount();

    /** Returns the appropriate thread to fetch using the LSQ count policy. */
    ThreadID lsqCount();

    /** Returns the appropriate thread to fetch using the branch count
     * policy. */
    ThreadID branchCount();

    /** Pipeline the next I-cache access to the current one. */
    std::vector<RequestPtr> pipelineIcacheAccesses(ThreadID tid,
                                Addr start_addr,
                                const FtqFetchInfo& fetch_info,
                                bool is_prefetch=false);

    /** Profile the reasons of fetch stall. */
    void profileStall(ThreadID tid);

    void advance(void);
    void sendInstToNextStage(TimeBuffer<FetchStruct>::wire& fromQ,
                             TimeBuffer<FetchStruct>::wire& toQ);
    void setSquash(TimeBuffer<FetchStruct>::wire& instQ);
    void setSquash(TimeBuffer<std::vector<RequestPtr>>::wire& reqs);
    void setSquash(TimeBuffer<std::vector<DynInstPtr>>::wire& insts);
    void setSquash(TimeBuffer<BpuToFetchStruct>::wire& p_ftq_info);
    void clear(TimeBuffer<std::vector<RequestPtr>>::wire& reqs);
    void clear(std::unordered_map<RequestPtr, uint8_t *>& req_buffers);
    void clear(RequestPtr req,
               std::unordered_map<RequestPtr, uint8_t *>& req_buffers);
    bool isValid(TimeBuffer<std::vector<RequestPtr>>::wire& reqs);
    bool isValid(std::vector<RequestPtr>& reqs);
    bool isValid(RequestPtr p_req);
    bool predecode(std::vector<RequestPtr>& reqs, unsigned ftq_idx);
    void precheck(FtqFetchInfo& fetch_info, std::vector<DynInstPtr>& insts,
                  std::vector<DynInstPtr>& valid_insts,
                  std::vector<DynInstPtr>& invalid_insts);
    void sendInstToDecode(void);
    bool eraseReq(std::vector<RequestPtr>& reqs,
                  FetchReqStatus stat=FetchReqStatus::Finish);
    bool eraseReq(const RequestPtr &mem_req,
                  FetchReqStatus stat=FetchReqStatus::Finish);
    void pushReqToList(const RequestPtr &mem_req, const PCStateBase& this_pc);
    void setReqStatus(const RequestPtr &mem_req, FetchReqStatus stat);
    void setReqStatus(const std::vector<RequestPtr> &reqs,
                      FetchReqStatus stat);
    void setReqFault(const RequestPtr &mem_req, Fault fault);
    void setReqFetchInfo(const RequestPtr &mem_req,
                       const FtqFetchInfo& fetch_info);
    void setReqFetchInfo(const std::vector<RequestPtr> &reqs,
                       const FtqFetchInfo& fetch_info);
    bool isStatus(const RequestPtr &mem_req, FetchReqStatus stat);
    bool isStatus(const std::vector<RequestPtr> &reqs, FetchReqStatus stat);
    bool isStatus(const std::vector<RequestPtr> &reqs,
                  const std::vector<FetchReqStatus> stats);
    bool hasStatus(const std::vector<RequestPtr> &reqs, FetchReqStatus stat);
    void updateReqList(void);
    void updateIcacheDataMap(void);
    PCStateBase& getReqPc(const RequestPtr &mem_req);
    Fault& getReqFault(const RequestPtr &mem_req);
    FtqFetchInfo& getReqFetchInfo(const RequestPtr &mem_req);
    bool isEmpty(RequestPtr& ptr);
    bool isEmpty(std::vector<RequestPtr>& ptr);
    bool isEmpty(FetchStruct& insts);
    bool isEmpty(std::vector<DynInstPtr>& insts);
    template<typename T>
    bool isBackPressure(TimeBuffer<T>& buffer) {
        return !isEmpty(*(buffer.getWire(-1)));
    }
    void tick0Squash(void);
    void tick1Squash(void);
    void tick2Squash(void);
    void tick3Squash(void);
    void intraSquash(ThreadID tid, std::vector<unsigned>& squashed_ftq_idxs);
    void fetchQueueSquash(void);
    void sendInfoToBpu(void);
    void predecodeSquash(std::vector<DynInstPtr>& insts);
    PCStateBase* newPcState(Addr pc);// { return cpu->newPCState(pc); }
    Addr readNpc(const PCStateBase& pc);

  private:
    /** Pointer to the O3CPU. */
    CPU *cpu;

    /** Time buffer interface. */
    TimeBuffer<TimeStruct> *timeBuffer;

    /** Wire to get decode's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromDecode;
    TimeBuffer<TimeStruct>::wire fromDecodeToIfu;

    /** Wire to get rename's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromRename;

    /** Wire to get iew's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromIEW;

    /** Wire to get commit's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromCommit;
    TimeBuffer<TimeStruct>::wire fromCommitToIfu;

    //Might be annoying how this name is different than the queue.
    /** Wire used to write any information heading to decode. */
    TimeBuffer<FetchStruct>::wire toDecode;

    /** timebuffer queue in F3 to FTQ*/
    TimeBuffer<FetchToBpuStruct> toBpuQueue;
    TimeBuffer<FetchToBpuStruct>::wire toBpu;
    TimeBuffer<FetchToBpuStruct>::wire fromIfu;
    TimeBuffer<BpuToFetchStruct> toIfuQueue;
    TimeBuffer<BpuToFetchStruct>::wire fromBpu;
    /** buffer to store FTQ->IFU info */
    std::deque<FtqFetchInfo> ftqFetchInfoBuffer;
    uint32_t ftqInfoBufferMaxSize;
    /** delay between ftq to ifu */
    uint32_t bpuToIfuDelay;

    /** timebuffer queue in fetch stages to connect*/
    TimeBuffer<std::vector<RequestPtr>> f0Queue;
    TimeBuffer<std::vector<RequestPtr>> f1Queue;
    TimeBuffer<std::vector<RequestPtr>> f2Queue;
    TimeBuffer<std::vector<DynInstPtr>> decodeInstQueue;
    TimeBuffer<std::vector<RequestPtr>>::wire fromF0;
    TimeBuffer<std::vector<RequestPtr>>::wire toF1;
    TimeBuffer<std::vector<RequestPtr>>::wire fromF1;
    TimeBuffer<std::vector<RequestPtr>>::wire toF2;
    TimeBuffer<std::vector<RequestPtr>>::wire fromF2;
    TimeBuffer<std::vector<RequestPtr>>::wire toF3;
    TimeBuffer<std::vector<DynInstPtr>>::wire fromF2Insts;
    TimeBuffer<std::vector<DynInstPtr>>::wire toF3Insts;

    /** fetch stages block flag */
    bool f0BlockFlag;
    bool f1BlockFlag;
    bool f2BlockFlag;
    bool f3BlockFlag;

    //precheck result to feedback to FTQ
    PreDecSquash preDecSquash;
    PdInfo pdInfo;

    /** list of squashed icache reqs */
    std::unordered_map<RequestPtr, ReqInfoPtr> fetchReqList;
    /** map ftq_idx to fetch req */
    std::unordered_map<unsigned, std::vector<RequestPtr>> ftqIdxReqs;
    /** map ftq_idx to fetch req */
    std::unordered_map<unsigned, std::pair<Addr, Addr>> idxToOffset;

    /** BPredUnit. */
    DecoupledBPU *branchPred;

    std::unique_ptr<PCStateBase> pc[MaxThreads];

    Addr fetchOffset[MaxThreads];
    Addr preFtqInfoEndAddr[MaxThreads];

    StaticInstPtr macroop[MaxThreads];

    /** Can the fetch stage redirect from an interrupt on this instruction? */
    bool delayedCommit[MaxThreads];

    /** Memory request used to access cache. */
    std::vector<RequestPtr> memReq[MaxThreads];

    /** Memory request new to access cache. */
    std::vector<RequestPtr> fetchReq[MaxThreads];
    std::vector<RequestPtr> prefetchReq[MaxThreads];

    /** Variable that tracks if fetch has written to the time buffer this
     * cycle. Used to tell CPU if there is activity this cycle.
     */
    bool wroteToTimeBuffer;

    /** Tracks how many instructions has been fetched this cycle. */
    int numInst;

    /** status for ticks */
    bool status_change;

    /** Source of possible stalls. */
    struct Stalls
    {
        bool decode;
        bool drain;
    };

    /** Tracks which stages are telling fetch to stall. */
    Stalls stalls[MaxThreads];

    /** Decode to fetch delay. */
    Cycles decodeToFetchDelay;

    /** Rename to fetch delay. */
    Cycles renameToFetchDelay;

    /** IEW to fetch delay. */
    Cycles iewToFetchDelay;

    /** Commit to fetch delay. */
    Cycles commitToFetchDelay;

    /** The width of fetch in instructions. */
    unsigned fetchWidth;

    /** The width of decode in instructions. */
    unsigned decodeWidth;

    /** Is the cache blocked?  If so no threads can access it. */
    bool cacheBlocked;

    /** The packet that is waiting to be retried. */
    std::queue<PacketPtr> retryPktQ;

    /** The thread that is waiting on the cache to tell fetch to retry. */
    ThreadID retryTid;

    /** Cache block size. */
    unsigned int cacheBlkSize;

    /** The size of the fetch buffer in bytes. The fetch buffer
     *  itself may be smaller than a cache line.
     */
    unsigned fetchBufferSize;

    /** Mask to align a fetch address to a fetch buffer boundary. */
    Addr fetchBufferMask;

    /** The fetch data that is being fetched and buffered. */
    std::unordered_map<RequestPtr, uint8_t *> fetchBuffer[MaxThreads];

    /** The PC of the first instruction loaded into the fetch buffer. */
    Addr fetchBufferPC[MaxThreads];

    /** The size of the fetch queue in micro-ops */
    unsigned fetchQueueSize;

    /** Queue of fetched instructions. Per-thread to prevent HoL blocking. */
    std::deque<DynInstPtr> fetchQueue[MaxThreads];
    /** insts from predecode*/
    std::vector<DynInstPtr> fetchInstQueue;

    /** Whether or not the fetch buffer data is valid. */
    bool fetchBufferValid[MaxThreads];

    /** Size of instructions. */
    int instSize;

    /** Icache stall statistics. */
    Counter lastIcacheStall[MaxThreads];

    /** List of Active Threads */
    std::list<ThreadID> *activeThreads;

    /** Number of threads. */
    ThreadID numThreads;

    /** Number of threads that are actively fetching. */
    ThreadID numFetchingThreads;

    /** Thread ID being fetched. */
    ThreadID threadFetched;

    /** Checks if there is an interrupt pending.  If there is, fetch
     * must stop once it is not fetching PAL instructions.
     */
    bool interruptPending;

    /** Instruction port. Note that it has to appear after the fetch stage. */
    IcachePort icachePort;

    /** Set to true if a pipelined I-cache request should be issued. */
    bool issuePipelinedIfetch[MaxThreads];

    /** Event used to delay fault generation of translation faults */
    FinishTranslationEvent finishTranslationEvent;

    /** count ftb alias */
    unsigned ftb_false_hit;
  protected:
    struct FetchStatGroup : public statistics::Group
    {
        FetchStatGroup(CPU *cpu, FetchStage *fetch);
        // @todo: Consider making these
        // vectors and tracking on a per thread basis.
        /** Stat for total number of cycles stalled wait FTQ cmd. */
        statistics::Scalar ftqStallCycles;
        /** Stat for total number of cycles stalled due to an icache miss. */
        statistics::Scalar icacheStallCycles;
        /** Stat for total number of fetched instructions. */
        statistics::Scalar insts;
        /** Total number of branch instruction . */
        statistics::Scalar branches;
        /** Stat for total number of predicted branches. */
        statistics::Scalar predictedBranches;
        /** Stat for total number of cycles spent fetching. */
        statistics::Scalar cycles;
        /** Stat for total number of cycles spent squashing. */
        statistics::Scalar squashCycles;
        /** Stat for total number of cycles spent waiting for translation */
        statistics::Scalar tlbCycles;
        /** Stat for total number of cycles
         *  spent blocked due to other stages in
         * the pipeline.
         */
        statistics::Scalar idleCycles;
        /** Total number of cycles spent blocked. */
        statistics::Scalar blockedCycles;
        /** Total number of cycles spent in any other state. */
        statistics::Scalar miscStallCycles;
        /** Total number of cycles spent in waiting for drains. */
        statistics::Scalar pendingDrainCycles;
        /** Total number of stall cycles caused by no active threads to run. */
        statistics::Scalar noActiveThreadStallCycles;
        /** Total number of stall cycles caused by pending traps. */
        statistics::Scalar pendingTrapStallCycles;
        /** Total number of stall cycles
         *  caused by pending quiesce instructions. */
        statistics::Scalar pendingQuiesceStallCycles;
        /** Total number of stall cycles caused by I-cache wait retrys. */
        statistics::Scalar icacheWaitRetryStallCycles;
        /** Stat for total number of fetched cache lines. */
        statistics::Scalar cacheLines;
        /** Stat for total number of fetched cache pipeline0 request. */
        statistics::Scalar cacheLine0_req;
        /** Stat for total number of fetched cache pipeline1 request. */
        statistics::Scalar cacheLine1_req;
        /** Total number of outstanding icache accesses that were dropped
         * due to a squash.
         */
        statistics::Scalar icacheSquashes;
        /** Total number of outstanding tlb accesses that were dropped
         * due to a squash.
         */
        statistics::Scalar tlbSquashes;
        /** Distribution of number of instructions fetched each cycle. */
        statistics::Distribution nisnDist;
        /** Rate of how often fetch was idle. */
        statistics::Formula idleRate;
        /** Number of branch fetches per cycle. */
        statistics::Formula branchRate;
        /** Number of instruction fetched per cycle. */
        statistics::Formula rate;
    } fetchStats;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_FETCH_HH__

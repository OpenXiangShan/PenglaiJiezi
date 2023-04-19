/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * Copyright (c) 2020 Barkhausen Institut
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

#ifndef __ARCH_RISCV_TABLE_WALKER_HH__
#define __ARCH_RISCV_TABLE_WALKER_HH__

#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/tlb.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "params/RiscvPagetableWalker.hh"
#include "sim/clocked_object.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

class ThreadContext;

namespace RiscvISA
{
    class Walker : public ClockedObject
    {
      protected:
        // Port for accessing memory
        class WalkerPort : public RequestPort
        {
          public:
            WalkerPort(const std::string &_name, Walker * _walker) :
                  RequestPort(_name, _walker), walker(_walker)
            {}

          protected:
            Walker *walker;

            bool recvTimingResp(PacketPtr pkt);
            void recvReqRetry();
        };

        friend class WalkerPort;
        WalkerPort port;

        // State to track each walk of the page table
        class WalkerState
        {
          friend class Walker;
          private:
            enum State
            {
                Ready,
                Waiting,
                Translate,
            };

            struct RequestorState
            {
                ThreadContext *tc;
                RequestPtr req;
                BaseMMU::Translation *translation;
                Fault fault;
                bool squashed;

                RequestorState()
                {
                    tc = nullptr;
                    req = nullptr;
                    translation = nullptr;
                    fault = NoFault;
                    squashed = false;
                }
                RequestorState(ThreadContext *tc, RequestPtr req, BaseMMU::Translation *translation) :
                    tc(tc), req(req), translation(translation), fault(NoFault), squashed(false)
                {}

                void markSquash() { squashed = true; }
            };


            std::list<RequestorState> requestors;


          protected:
            Walker *walker;
            RequestPtr mainReq;  // req is renamed to main req
            State state;
            State nextState;
            int level;
            unsigned inflight;
            TlbEntry entry;
            TlbEntry inl2_entry;
            PacketPtr read;
            std::vector<PacketPtr> writes;
            Fault mainFault;
            BaseMMU::Mode mode;
            SATP satp;
            STATUS status;
            PrivilegeMode pmode;
            bool functional;
            bool timing;
            bool retrying;
            bool started;
            bool squashed;
            bool next_line;
            Addr nextline_Read;
            int nextline_level;
            TlbEntry nextline_entry;
            Addr nextline_vaddr;

            Addr nextline_level_mask;
            Addr nextline_shift;
            Addr tlb_vaddr;
            Addr tlb_ppn;
            Addr tlb_size_pte;
            bool open_nextline;
            bool auto_nextline_sign;

          public:
            WalkerState(Walker * _walker, BaseMMU::Translation *_translation,
                        const RequestPtr &_req, bool _isFunctional = false) :
                walker(_walker), mainReq(_req), state(Ready),
                nextState(Ready), level(0), inflight(0),
                functional(_isFunctional), timing(false),
                retrying(false), started(false), squashed(false)
            {
                requestors.emplace_back(nullptr, _req, _translation);
            }
            void initState(ThreadContext * _tc, BaseMMU::Mode _mode,
                           bool _isTiming = false);

            std::pair<bool, Fault> tryCoalesce(
                ThreadContext *_tc, BaseMMU::Translation *translation,
                const RequestPtr &req, BaseMMU::Mode mode, bool from_l2tlb,
                Addr asid);

            Fault startWalk(Addr ppn, int f_level, bool from_l2tlb,
                            bool OpenNextline, bool autoOpenNextline);
            Fault startFunctional(Addr &addr, unsigned &logBytes,
                                  bool OpenNextline, bool autoOpenNextline);
            bool recvPacket(PacketPtr pkt);
            unsigned numInflight() const;
            bool isRetrying();
            bool wasStarted();
            bool isTiming();
            void retry();
            std::string name() const {return walker->name();}

            bool anyRequestorSquashed() const;
            bool allRequestorSquashed() const;

          private:
            void setupWalk(Addr ppn, Addr vaddr, int f_level, bool from_l2tlb,
                           bool OpenNextline, bool autoOpenNextline);
            Fault stepWalk(PacketPtr &write);
            void sendPackets();
            void endWalk();
            Fault pageFault(bool present);
            Fault pageFaultOnRequestor(RequestorState &requestor);
        };

        struct L2TlbState
        {
              RequestPtr req;
              ThreadContext *tc;

              BaseMMU::Translation *translation;
              BaseMMU::Mode mode;
              Addr Paddr;
              TlbEntry entry;

        };
        std::list<L2TlbState> L2TLBrequestors;

        friend class WalkerState;
        // State for timing and atomic accesses (need multiple per walker in
        // the case of multiple outstanding requests in timing mode)
        std::list<WalkerState *> currStates;
        // State for functional accesses (only need one of these per walker)
        WalkerState funcState;

        struct WalkerSenderState : public Packet::SenderState
        {
            WalkerState * senderWalk;
            WalkerSenderState(WalkerState * _senderWalk) :
                senderWalk(_senderWalk) {}
        };

      public:
        // Kick off the state machine.
        Fault start(Addr ppn, ThreadContext *_tc,
                    BaseMMU::Translation *translation, const RequestPtr &req,
                    BaseMMU::Mode mode, bool pre = false, int f_level = 2,
                    bool from_l2tlb = false, Addr asid = 0);

        void doL2TLBHitSchedule(const RequestPtr &req, ThreadContext *tc,
                                BaseMMU::Translation *translation,
                                BaseMMU::Mode mode, Addr Paddr,
                                const TlbEntry &entry);



        std::pair<bool, Fault> tryCoalesce(ThreadContext *_tc,
                                           BaseMMU::Translation *translation,
                                           const RequestPtr &req,
                                           BaseMMU::Mode mode, bool from_l2tlb,
                                           Addr asid);

        // Fault perm_check ();

        Fault startFunctional(ThreadContext * _tc, Addr &addr,
                unsigned &logBytes, BaseMMU::Mode mode);
        Port &getPort(const std::string &if_name,
                      PortID idx=InvalidPortID) override;

      protected:
        // The TLB we're supposed to load.
        TLB * tlb;
        System * sys;
        PMAChecker * pma;
        PMP * pmp;
        RequestorID requestorId;

        // The number of outstanding walks that can be squashed per cycle.
        unsigned numSquashable;
        bool ptwSquash;
        bool OpenNextline;
        bool autoOpenNextline;

        Tick squashHandleTick;

        // Wrapper for checking for squashes before starting a translation.
        //void startWalkWrapper();

        // Checking for squashes
        //void handlePendingSquash();

        void dol2TLBHit();

        /**
         * Event used to call startWalkWrapper.
         **/


        EventFunctionWrapper doL2TLBHitEvent;

        // Functions for dealing with packets.
        bool recvTimingResp(PacketPtr pkt);
        void recvReqRetry();
        bool sendTiming(WalkerState * sendingState, PacketPtr pkt);
        bool pre_ptw;

      public:

        void setTLB(TLB * _tlb)
        {
            tlb = _tlb;
        }

        using Params = RiscvPagetableWalkerParams;

        Walker(const Params &params) :
            ClockedObject(params), port(name() + ".port", this),
            funcState(this, NULL, NULL, true), tlb(NULL), sys(params.system),
            pma(params.pma_checker),
            pmp(params.pmp),
            requestorId(sys->getRequestorId(this)),
            numSquashable(params.num_squash_per_cycle),
            ptwSquash(params.ptwSquash),
            OpenNextline(params.OpenNextline),
            autoOpenNextline(true),
            doL2TLBHitEvent([this]{dol2TLBHit();},name())
        {
        }
    };

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGE_TABLE_WALKER_HH__

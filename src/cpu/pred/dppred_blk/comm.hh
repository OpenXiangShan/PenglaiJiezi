/*
 * Copyright (c) 2023 Todo
 *
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

#ifndef __CPU_PRED_COMM_HH__
#define __CPU_PRED_COMM_HH__

#include <vector>
#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace branch_prediction
{
    enum JmpType
    {
      JAL = 0,
      JALR,
      CALL,
      RET
    };

    struct PdInfo
    {
      bool valid=false;
      bool false_hit;
      unsigned ftqIdx;
      bool validInstr[MaxInstrsPerBlock];
      bool brMask[MaxInstrsPerBlock];
      bool hasJmp=false;
      bool isDirect=false;
      unsigned jmpAttr;
      unsigned jmpOffset;
      bool rvcMask[MaxInstrsPerBlock];
      Addr jmpTarget;
    };

    struct BTBEntry
    {
      struct SlotEntry
      {
        /** Whether or not this slot is valid. */
        bool valid = false;

        /** this slot target*/
        Addr target = 0;

        /** offset, 0~15 */
        int8_t offset = 0;

        /** whether this slot is unconditional*/
        bool sharing = false;

        bool always_taken = false;
      };

      /** The entry's tag. */
      Addr tag = 0;

      /** The entry's target. */
      SlotEntry slot[MaxNumBr];

      /** The entry's thread id. */
      ThreadID tid = 0;

      bool isCall = false;

      bool isRet = false;

      bool isJalr = false;

      Addr fallThruAddr = 0;

      /** Whether or not the entry is valid. */
      bool valid = false;
    };


    struct BTBBranchInfo
    {
      bool btb_hit = false;
      BTBEntry hitBtbEntry;
    };


    struct PreDecSquash
    {
      bool squash=false;
      unsigned ftqIdx;
      InstSeqNum seqNum;
      Addr branchAddr;
      Addr target;
      bool branchTaken;
      gem5::o3::DynInstPtr squashInst;
    };

    struct FetchToBpuStruct
    {
      PreDecSquash preDecSquash; //predecode flush
      PdInfo pdInfo; //write to ftq
      bool bpuBlock = false;
      bool bpuPrefetchBlock = false;

      struct BackendSquash
      {
        bool squash = false;
        bool squashItSelf = false;

        /**all squashed info, including squash_itself/after, ftqIdx*/
        gem5::o3::DynInstPtr squashInst;

        /**including branchTaken, ftqIdx*/
        gem5::o3::DynInstPtr mispredictInst;

        /// Was the branch taken or not
        bool branchTaken;

        /// branch target
        std::unique_ptr<PCStateBase> pc;
      };
      BackendSquash backendSquash;

      std::vector<gem5::o3::DynInstPtr> cmtInfo;
      bool interruptPending;
      bool clearInterrupt;
    };

    struct BpuToFetchStruct
    {
      struct InstBlkFetchInfo
      {
        bool valid = false; //has backward pressure
        bool confidence = false;
        unsigned ftqIdx;
        Addr startAddr;
        Addr endAddr;  //next block startAddr if not taken
        Addr target;
        bool predTaken;
        unsigned cfiIndex;  //taken position(0~15)
      };

      struct InstBlkPrefetchInfo
      {
        bool valid = false; //has backward pressure
        Addr startAddr;
      };

      InstBlkFetchInfo instBlkFetchInfo;
      bool intra_squash = false;
      std::vector<unsigned> intra_squashed_ftq_idxs;
      InstBlkPrefetchInfo instBlkPrefetchInfo;
    };

} // namespace o3
} // namespace gem5

#endif //__CPU_PRED_COMM_HH__

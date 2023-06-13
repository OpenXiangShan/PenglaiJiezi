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

#ifndef __CPU_PRED_DECOUPLED_BPU_HH__
#define __CPU_PRED_DECOUPLED_BPU_HH__

#include <vector>

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "cpu/pred/dppred_blk/ftq.hh"
#include "cpu/pred/dppred_blk/tage_sc_i_blk.hh"
#include "cpu/timebuf.hh"
#include "params/DecoupledBPU.hh"

namespace gem5
{

namespace branch_prediction
{

class DecoupledBPU: public SimObject
{
  public:

    DecoupledBPU(const DecoupledBPUParams &params);

    //called by fetch
    void setStartAddr(Addr startAddr);

    //called by fetch, to get ras retAddr
    Addr getRetAddr(unsigned ftq_idx);

    //called by fetch
    void  setBpuToFetchQueue(TimeBuffer<BpuToFetchStruct> *time_buffer);

    //called by fetch
    void  setFetchToBpuQueue(TimeBuffer<FetchToBpuStruct> *time_buffer);


  protected:
    FTQ::FtqEntry ftq_entry_pred_s1;
    FTQ::FtqEntry ftq_entry_pred_s2;
    FTQ::FtqEntry ftq_entry_pred_s3;
    FTQ::FtqEntry ftq_entry_fetch;
    FTQ::FtqEntry ftq_entry_prefetch;
    TAGE_SC_I_BLK::SquashInfo squash_info;
    TAGE_SC_I_BLK::UpdateInfo update_info;
    bool trace_confidence = false;

    TAGE_SC_I_BLK *tage_sc_i_blk;
    FTQ *ftq;
    bool interrupt_pending;

  private:
    TimeBuffer<FetchToBpuStruct>::wire fromFetch;
    TimeBuffer<BpuToFetchStruct>::wire toFetch;
    struct BpuPipeStruct
    {
      FTQ::FtqEntry ftq_entry;
      bool pred_enable = false;
      bool slot1_is_indir = false;
      bool slot1_is_call = false;
      bool slot1_is_ret = false;
      bool confidence[MaxNumBr];
      bool pred_isCond[MaxNumBr];
      bool pred_cond_taken[MaxNumBr];
      BpuPipeStruct(){
        for (unsigned i = 0; i < MaxNumBr; ++i){
          pred_isCond[i] = false;
          pred_cond_taken[i] = false;
        }
      }
    };

    TimeBuffer<BpuPipeStruct> S1Queue; //register for s1 pred info
    TimeBuffer<BpuPipeStruct>::wire fromS1;
    TimeBuffer<BpuPipeStruct>::wire toS2;

    TimeBuffer<BpuPipeStruct> S2Queue; //register for s2 pred info
    TimeBuffer<BpuPipeStruct>::wire fromS2;
    TimeBuffer<BpuPipeStruct>::wire toS3;

    struct DecoupledBPUStats : public statistics::Group
    {
        DecoupledBPUStats(statistics::Group *parent);
        /** Stat for number of conditional branches committed. */
        statistics::Scalar condCommitted;
        /** Stat for number of indir branches committed. */
        statistics::Scalar indirCommitted;
        /** Stat for number of ret branches committed. */
        statistics::Scalar retCommitted;
        /** Stat for number of conditional branches predicted incorrectly. */
        statistics::Scalar condIncorrect;
        /** Stat for number of indir branches predicted incorrectly. */
        statistics::Scalar indirIncorrect;
        /** Stat for number of ret branches predicted incorrectly. */
        statistics::Scalar retIncorrect;
        /** Stat for number of ubtb commit hit. */
        statistics::Scalar ubtbCommitHit;
        /** Stat for number of ubtb commit miss. */
        statistics::Scalar ubtbCommitMiss;
        /** Stat for number of ubtb lookups. */
        statistics::Scalar ubtbLookups;
        /** Stat for number of s2 redirect. */
        statistics::Scalar s2_redir;
        /** Stat for number of s3 redirect. */
        statistics::Scalar s3_redir;
        /** Stat for number of condIncorrect for btb miss. */
        statistics::Scalar condIncorrect_for_btb_miss;
        /** Stat for number of condIncorrect for short forward branch. */
        statistics::Scalar condIncorrect_for_sfb;
        /** Stat for number of confidence high but misp. */
        statistics::Scalar confHighMisp;
        /** Stat for number of confidence low and misp. */
        statistics::Scalar confLowMisp;
        /** Stat for number of confidence high and right. */
        statistics::Scalar confHighRight;
        /** Stat for number of confidence low but right. */
        statistics::Scalar confLowRight;
        /** Stat for number of BTB lookups. */
        statistics::Scalar BTBLookups;
        /** Stat for number of BTB hits. */
        statistics::Scalar BTBHits;
        statistics::Scalar updateNum;
        /** Stat for the ratio between BTB hits and BTB lookups. */
        statistics::Formula BTBHitRatio;
    } stats;

  public:
    void tick();
    void advance();
    BTBBranchInfo get_ftb_for_precheck(unsigned ftq_idx);
    unsigned new_btb_gen(Addr start_addr, BTBEntry old_btb_entry,
                          PdInfo pdInfo, FTQ::CfiInfo cfi_info,
                          Addr target, bool hit,
                          bool mispred[MaxInstrsPerBlock],
                          BTBEntry &new_btb_entry,
                          bool &slot0_false_empty);

};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_TAGE_SC_I_BLK_HH__

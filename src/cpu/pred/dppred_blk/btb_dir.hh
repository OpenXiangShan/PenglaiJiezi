/*
 * Copyright (c) 2011-2012, 2014 ARM Limited
 * Copyright (c) 2010 The University of Edinburgh
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
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#ifndef __CPU_PRED_BTB_DIR_HH__
#define __CPU_PRED_BTB_DIR_HH__

#include <deque>
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/dppred_blk/btb_blk.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/static_inst.hh"
#include "params/BTB_DIR.hh"
#include "sim/probe/pmu.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

/**
 * Basically a wrapper class to hold both the branch predictor
 * and the BTB.
 */
class BTB_DIR : public SimObject
{
  public:
    /**
     * @param params The params object, that has the size of the BP and BTB.
     */
    BTB_DIR(const BTB_DIRParams &p);

    virtual void lookup(ThreadID tid, Addr instPC, void * &bp_history,
                        bool* confidence, bool *isCond, bool* pred_taken,
                        bool* isAlwaysTaken) = 0;

    bool predict(Addr &startAddr, Addr &endAddr,
                 Addr &target, unsigned &pred_offset,
                 ThreadID tid, void * &bp_history,
                 bool &slot1_is_indir, bool &slot1_is_ret,
                 bool &slot1_is_call, bool *isCond,
                 bool *pred_taken, bool *confidence);

    /** recover ghr, bp_history is the snapshot when predict*/
    virtual void squash_recover(ThreadID tid, void * &bp_history,
                                bool actual_taken, bool isCondMisp,
                                unsigned numBrBefore) = 0;

    /**release mem space when squash*/
    virtual void dir_free_mem(void * &bp_history) = 0;

    /** update dir predictor*/
    virtual void dir_update(Addr startAddr, ThreadID tid,
                            bool *actual_taken, bool* isCond,
                            void * &bp_history) = 0;

    bool btb_alloc_update_lookup(Addr startAddr,
                    ThreadID tid, BTBEntry &hit_btb_entry);

    /** update btb & dir predictor*/
    void update(Addr startAddr, ThreadID tid, bool *actual_taken, bool* isCond,
                void * &bp_history, BTBEntry new_btb_entry,
                bool allocate_btb, bool update_btb, bool false_hit,
                bool slot0_false_empty);

    /** memory free for btb & dir predictor*/
    void free_mem(void * &bp_history);

  private:
    /** Pointer to the history object passed back from the branch
         * predictor.  It is used to update or restore state of the
         * branch predictor.
    */

    /** Number of the threads for which the branch history is maintained. */
    const unsigned numThreads;

  protected:
    /** Number of bits to shift instructions by for predictor addresses. */
    const unsigned instShiftAmt;

    /** 32B */
    const unsigned predBlkSize;

    const unsigned numBr;

  public:

    /** The BTB. */
    BTB_BLK BTB;

    struct DIRBTBBranchInfo
    {
        BTBBranchInfo *btbBranchInfo;
        void *dirBranchInfo=nullptr;

        DIRBTBBranchInfo(){
          btbBranchInfo = new BTBBranchInfo;
        }

        ~DIRBTBBranchInfo(){
          delete btbBranchInfo;
        }

    };

}; // namespace branch_prediction

} // namespace gem5
}

#endif // __CPU_PRED_BPRED_UNIT_HH__

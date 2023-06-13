/*
 * Copyright (c) 2011-2012, 2014 ARM Limited
 * Copyright (c) 2010 The University of Edinburgh
 * Copyright (c) 2012 Mark D. Hill and David A. Wood
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

#include "cpu/pred/dppred_blk/btb_dir.hh"
#include <algorithm>
#include "arch/generic/pcstate.hh"
#include "base/compiler.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "debug/Branch.hh"
#include "debug/DecoupledBPU.hh"

namespace gem5
{

namespace branch_prediction
{

BTB_DIR::BTB_DIR(const BTB_DIRParams &params)
    : SimObject(params),
      numThreads(params.numThreads),
      instShiftAmt(params.instShiftAmt),
      predBlkSize(params.predBlkSize),
      numBr(params.numBr),
      BTB(params.numBr,
          params.BTBEntries,
          params.BTBways,
          params.BTBTagSize,
          params.instShiftAmt,
          params.numThreads)
{
}



bool
BTB_DIR::predict(Addr &startAddr, Addr &endAddr,
                 Addr &target, unsigned &pred_offset,
                 ThreadID tid, void * &bp_history,
                 bool &slot1_is_indir, bool &slot1_is_ret,
                 bool &slot1_is_call, bool *isCond,
                 bool *pred_taken, bool *confidence)
{

    DIRBTBBranchInfo *bi = new DIRBTBBranchInfo;

    BTBEntry *btb_hit_entry = BTB.lookup(startAddr, tid);
    bi->btbBranchInfo->btb_hit = BTB.valid(startAddr, tid);
    bi->btbBranchInfo->hitBtbEntry = *btb_hit_entry;

    bool isAlwaysTaken[numBr];
    bool dir_pred[numBr];
    bool predTaken;

    if (!BTB.valid(startAddr, tid)){
        isCond[0] = false;
        isCond[1] = false;
        isAlwaysTaken[0] = false;
        isAlwaysTaken[1] = false;
        confidence[0] = false;
        confidence[1] = false;
    }else{
        isCond[0] = btb_hit_entry->valid && btb_hit_entry->slot[0].valid;
        isCond[1] = btb_hit_entry->valid && btb_hit_entry->slot[1].valid &&
                    btb_hit_entry->slot[1].sharing;
        isAlwaysTaken[0] = btb_hit_entry->valid &&
                           btb_hit_entry->slot[0].valid &&
                           btb_hit_entry->slot[0].always_taken;
        isAlwaysTaken[1] = btb_hit_entry->valid &&
                           btb_hit_entry->slot[1].valid &&
                           btb_hit_entry->slot[1].always_taken &&
                           btb_hit_entry->slot[1].sharing;
        if (!isCond[0]){
            confidence[0] = true;
        }else{
            confidence[0] = false;
        }
        if (!isCond[1] && !btb_hit_entry->isJalr){
            confidence[1] = true;
        }else{
            confidence[1] = false;
        }
        DPRINTF(Branch, "slot0 valid is %d\n",
                        btb_hit_entry->slot[0].valid);
        DPRINTF(Branch, "slot0 offset is %d\n",
                        btb_hit_entry->slot[0].offset);
        DPRINTF(Branch, "slot0 always taken is %d\n",
                        btb_hit_entry->slot[0].always_taken);
        DPRINTF(Branch, "slot0 sharing is %d\n",
                        btb_hit_entry->slot[0].sharing);
        DPRINTF(Branch, "slot1 valid is %d\n",
                        btb_hit_entry->slot[1].valid);
        DPRINTF(Branch, "slot1 offset is %d\n",
                        btb_hit_entry->slot[1].offset);
        DPRINTF(Branch, "slot1 always taken is %d\n",
                        btb_hit_entry->slot[1].always_taken);
        DPRINTF(Branch, "slot1 sharing is %d\n",
                        btb_hit_entry->slot[1].sharing);
        DPRINTF(Branch, "slot1 is call %d\n",
                        btb_hit_entry->isCall);
        DPRINTF(Branch, "slot0 cond is %d, slot1 cond is %d\n",
                        isCond[0], isCond[1]);
    }

    /** lookup dir predictor and speculative update GHR*/
    lookup(tid, startAddr, bi->dirBranchInfo, confidence,
            &isCond[0], &dir_pred[0], &isAlwaysTaken[0]);

    for (unsigned i = 0; i <= numBr-1; ++i){
        pred_taken[i] = dir_pred[i] || isAlwaysTaken[i];
    }
    slot1_is_indir = false;
    slot1_is_ret = false;
    slot1_is_call = false;

    if (!BTB.valid(startAddr, tid)){
        predTaken = false;
        pred_offset = 0;
        target = startAddr + predBlkSize;
        endAddr = startAddr + predBlkSize;
    } else {
        if (btb_hit_entry->slot[1].valid && !btb_hit_entry->slot[1].sharing){
            slot1_is_indir = btb_hit_entry->isJalr;
            slot1_is_ret = btb_hit_entry->isRet;
            slot1_is_call = btb_hit_entry->isCall;
        }
        if (btb_hit_entry->slot[0].valid && (dir_pred[0] ||
                        btb_hit_entry->slot[0].always_taken)){
            predTaken = true;
            pred_offset = btb_hit_entry->slot[0].offset;
            target = btb_hit_entry->slot[0].target;
        } else if (btb_hit_entry->slot[1].valid && (
                dir_pred[1] || !btb_hit_entry->slot[1].sharing ||
                btb_hit_entry->slot[1].always_taken)) {
            predTaken = true;
            pred_offset = btb_hit_entry->slot[1].offset;
            target = btb_hit_entry->slot[1].target;
        } else {
            predTaken = false;
            pred_offset = 0;
            target = btb_hit_entry->fallThruAddr;
        }
        endAddr = btb_hit_entry->fallThruAddr;
    }
    bp_history = (void*)(bi);
    return predTaken;
}

bool
BTB_DIR::btb_alloc_update_lookup(Addr startAddr,
                    ThreadID tid, BTBEntry &hit_btb_entry)
{
    return BTB.alloc_update_lookup(startAddr, tid, hit_btb_entry);
}

/** commit update*/
void
BTB_DIR::update(Addr startAddr, ThreadID tid, bool *actual_taken, bool* isCond,
                    void * &bp_history, BTBEntry new_btb_entry,
                    bool allocate_btb, bool update_btb, bool false_hit,
                    bool slot0_false_empty)
{
    DIRBTBBranchInfo *bi = static_cast<DIRBTBBranchInfo*>(bp_history);
    /** update btb */
    if (allocate_btb){
        BTB.allocate(startAddr, new_btb_entry, tid, false_hit);
    }else if (update_btb){
        BTB.update(startAddr, new_btb_entry, tid, slot0_false_empty);
    }

    /** update dir predictor*/
    dir_update(startAddr, tid, actual_taken, isCond, bi->dirBranchInfo);
}

void
BTB_DIR::free_mem(void * &bp_history)
{
    DIRBTBBranchInfo *bi = static_cast<DIRBTBBranchInfo*>(bp_history);
    dir_free_mem(bi->dirBranchInfo);
    delete bi;
}

} // namespace branch_prediction
} // namespace gem5

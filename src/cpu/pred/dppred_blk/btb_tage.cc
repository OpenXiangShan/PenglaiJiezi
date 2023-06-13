/*
 * Copyright (c) 2014 The University of Wisconsin
 *
 * Copyright (c) 2006 INRIA (Institut National de Recherche en
 * Informatique et en Automatique  / French National Research Institute
 * for Computer Science and Applied Mathematics)
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

#include "cpu/pred/dppred_blk/btb_tage.hh"
#include <cstdlib>
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/Fetch.hh"
#include "debug/Tage.hh"

namespace gem5
{

namespace branch_prediction
{
BTB_TAGE::BTB_TAGE(const BTB_TAGEParams &params) :
        BTB_DIR(params), tage(params.tage){
}

void
BTB_TAGE::squash_recover(ThreadID tid, void * &bp_history,
                        bool actual_taken, bool isCondMisp,
                        unsigned numBrBefore){
    /** recover global history and folded history */
    TageBranchInfo *bi = static_cast<TageBranchInfo*>(bp_history);
    TAGEBase_BLK::TageBranchInfo *tage_bi = bi->tageBranchInfo;
    tage->squash(tid, actual_taken, tage_bi, isCondMisp, numBrBefore);
}

void
BTB_TAGE::dir_free_mem(void * &bp_history){
    /** free memory*/
    TageBranchInfo *bi = static_cast<TageBranchInfo*>(bp_history);
    delete bi;
}

// PREDICTOR UPDATE
void
BTB_TAGE::dir_update(Addr startAddr, ThreadID tid,
                    bool *actual_taken, bool* isCond,
                        void * &bp_history)
{
    assert(bp_history);

    TageBranchInfo *bi = static_cast<TageBranchInfo*>(bp_history);
    TAGEBase_BLK::TageBranchInfo *tage_bi = bi->tageBranchInfo;

    //int nrand = random_mt.random<int>() & 3;
    int nrand = rand() & 3;
    //update stats info
    tage->updateStats(actual_taken, isCond, bi->tageBranchInfo);
    //update tage
    tage->condBranchUpdate(tid, startAddr, actual_taken,
                            isCond, tage_bi, nrand);
    DPRINTF(Tage, "Updating tables for block:%lx; \
            isCond: %d, %d; dir: %d, %d\n",
            startAddr, isCond[0], isCond[1],
            actual_taken[0], actual_taken[1]);
}

void
BTB_TAGE::lookup_only(ThreadID tid, Addr branch_pc, bool* cond_branch,
                void * &bp_history, bool* pred_taken, bool* confidence)
{
    TageBranchInfo *bi = new TageBranchInfo(*tage);
    tage->tagePredict(tid, branch_pc, cond_branch,
                        bi->tageBranchInfo, pred_taken);
    if (cond_branch[0]){
        if (bi->tageBranchInfo->provider[0] == 1){
            confidence[0] = abs(2 * bi->tageBranchInfo->hitBankCtr[0] + 1) > 5;
        }else{
            if (bi->tageBranchInfo->hitBank[0] > 0){
                confidence[0] = false;
            }else{
                confidence[0] = false;
                    //abs(2 * bi->tageBranchInfo->bimodalCtr[0] + 1) > 5;
            }
        }
    }
    if (cond_branch[1]){
        if (bi->tageBranchInfo->provider[1] == 1){
            confidence[1] = abs(2 * bi->tageBranchInfo->hitBankCtr[1] + 1) > 5;
        }else{
            if (bi->tageBranchInfo->hitBank[1] > 0){
                confidence[1] = false;
            }else{
                confidence[1] = false;
                    //abs(2 * bi->tageBranchInfo->bimodalCtr[1] + 1) > 5;
            }
        }
    }
    bp_history = (void*)(bi);
}

void
BTB_TAGE::lookup(ThreadID tid, Addr instPC, void * &bp_history,
                    bool* confidence, bool *isCond,
                    bool* pred_taken, bool* isAlwaysTaken)
{
    /** lookup tage predictor*/
    lookup_only(tid, instPC, isCond, bp_history, pred_taken, confidence);

    TageBranchInfo *bi = static_cast<TageBranchInfo*>(bp_history);

    DPRINTF(Tage, "Lookup block startAddr: %lx;\
            slot0 predict:%d; slot1 predict:%d\n",
            instPC, pred_taken[0], pred_taken[1]);

    bool final_pred_taken[numBr];
    for (unsigned i = 0; i <= numBr-1; ++i){
        final_pred_taken[i] = pred_taken[i] || isAlwaysTaken[i];
        //confidence[i] = confidence[i] || isAlwaysTaken[i];
    }

    /**speculatively update global history with tage predict result*/
    tage->updateHistories(tid, instPC, &final_pred_taken[0], isCond,
                            bi->tageBranchInfo, true);
}

unsigned
BTB_TAGE::getTageCtrBits(){
    return tage->tagTableCounterBits;
}

} // namespace branch_prediction
} // namespace gem5

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

/* @file
 * Implementation of a TAGE branch predictor
 */

#include "cpu/pred/dppred_blk/tage_sc_i_blk.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "debug/TAGE_SC_I_BLK.hh"

namespace gem5
{
namespace branch_prediction
{
/** 构造函数 */
TAGE_SC_I_BLK::TAGE_SC_I_BLK(const TAGE_SC_I_BLKParams &params):
                    SimObject(params),
                    enableSC(params.enableSC),
                    enableSLoopPred(params.enableSLoopPred),
                    ubtb_blk(params.ubtb_blk),
                    btb_tage(params.btb_tage),
                    ras_blk(params.ras_blk),
                    ittage_blk(params.ittage_blk),
                    new_sc_blk(params.new_sc_blk),
                    loop_pred_blk(params.loop_pred_blk){
}

//get ras ret addr
Addr
TAGE_SC_I_BLK::getRetAddr(TageSCIBranchInfo tage_sc_i_bInfo)
{
    return ras_blk->getRetAddr(tage_sc_i_bInfo.rasmeta);
}

bool
TAGE_SC_I_BLK::btb_alloc_update_lookup(Addr startAddr,
                        ThreadID tid, BTBEntry &hit_btb_entry){
    return btb_tage->btb_alloc_update_lookup(startAddr, tid, hit_btb_entry);
}

void
TAGE_SC_I_BLK::tick_s1(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool *confidence){
    bool taken_is_indir = false;
    bool taken_is_ret = false;
    bool taken_is_call = false;
    bool pred_isCond[MaxNumBr];
    bool pred_cond_taken[MaxNumBr];
    bool update_cond[MaxNumBr];
    for (unsigned br = 0; br < MaxNumBr; ++br){
        update_cond[br] = update_info.isCond[br] &&
                          !update_info.isAlwaysTaken[br];
    }
    if (!squash_info.is_squash && pred_enable){
        pred_taken = ubtb_blk->predict(startAddr, endAddr,
                          target, pred_offset,
                          tid, tage_sc_i_bInfo.ubtbBranchInfo,
                          taken_is_indir,
                          taken_is_ret, taken_is_call,
                          &pred_isCond[0], &pred_cond_taken[0],
                          confidence);
    }

    if (update_info.valid){ //taken or ftb hit
        ubtb_blk->update(update_info.startAddr,
                        update_info.tid,
                        &update_info.actual_taken[0],
                        &update_cond[0],
                        update_info.new_btb_entry,
                        update_info.tage_sc_i_bInfo.ubtbBranchInfo,
                        update_info.allocate_btb,
                        update_info.update_btb,
                        update_info.false_hit,
                        update_info.slot0_false_empty);
    }

    /*free mem*/
    if (free_tage_sc_i_bInfos.size() > 0){
        for (auto& free_entry : free_tage_sc_i_bInfos){
            ubtb_blk->free_mem(free_entry.ubtbBranchInfo);
        }
    }

}



void
TAGE_SC_I_BLK::tick_s2(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool &slot1_is_indir,
                        bool &slot1_is_call, bool &slot1_is_ret,
                        bool *pred_isCond, bool *pred_cond_taken,
                        bool *confidence){

    /** ftb & tage predict */
    void *lookup_tagebtbBranchInfo = NULL;
    bool taken_is_call = false;
    bool taken_is_ret = false;
    bool update_cond[MaxNumBr];
    for (unsigned br = 0; br < MaxNumBr; ++br){
        update_cond[br] = update_info.isCond[br] &&
                          !update_info.isAlwaysTaken[br];
    }

    /** squash recover */
    //tage ghr recover
    if (squash_info.is_squash){
        void *squash_tageBranchInfo =
            squash_info.tage_sc_i_bInfo.
                tagebtbBranchInfo->dirBranchInfo;
        btb_tage->squash_recover(squash_info.tid, squash_tageBranchInfo,
                                 squash_info.actual_taken,
                                 squash_info.isCond &&
                                 squash_info.isMispred,
                                 squash_info.numBrBefore);
        ras_blk->squash_recover(squash_info.tage_sc_i_bInfo.rasmeta,
                                squash_info.isCall,
                                squash_info.isRet,
                                squash_info.callPushAddr);
    }else{
        if (pred_enable){
            //btb+tage
            pred_taken = btb_tage->predict(startAddr, endAddr,
                            target, pred_offset,
                            tid, lookup_tagebtbBranchInfo,
                            slot1_is_indir, slot1_is_ret,
                            slot1_is_call, pred_isCond,
                            pred_cond_taken, confidence);
            tage_sc_i_bInfo.tagebtbBranchInfo =
                        static_cast<BTB_DIR::DIRBTBBranchInfo*>
                        (lookup_tagebtbBranchInfo);

            //ras
            //be carefull: there is call & ret instr in riscv
            if (!pred_isCond[0] || !pred_cond_taken[0]){
                taken_is_ret = slot1_is_ret;
                taken_is_call = slot1_is_call;
            }
            ras_blk->predict(taken_is_ret, target, taken_is_call,
                            endAddr, tage_sc_i_bInfo.rasmeta,
                            confidence);
        }
    }

    /** commit */
    if (update_info.valid){
        //allocate btb, update tage
        void *update_tagebtbBranchInfo =
                update_info.tage_sc_i_bInfo.tagebtbBranchInfo;

        btb_tage->update(update_info.startAddr, update_info.tid,
                            &update_info.actual_taken[0],
                            &update_cond[0],
                            update_tagebtbBranchInfo,
                            update_info.new_btb_entry,
                            update_info.allocate_btb,
                            update_info.update_btb,
                            update_info.false_hit,
                            update_info.slot0_false_empty);
    }

    /*free mem*/
    if (free_tage_sc_i_bInfos.size() > 0){
        for (auto& free_entry : free_tage_sc_i_bInfos){
            void *free_tagebtbBranchInfo = free_entry.tagebtbBranchInfo;
            btb_tage->free_mem(free_tagebtbBranchInfo);
            ras_blk->free_mem(free_entry.rasmeta);
        }
    }
}


void
TAGE_SC_I_BLK::tick_s3(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool slot1_is_indir,
                        bool slot1_is_ret, bool *pred_isCond,
                        bool *pred_cond_taken, bool &press,
                        unsigned ftqIdx, bool *confidence){

    bool update_cond[MaxNumBr];
    for (unsigned br = 0; br < MaxNumBr; ++br){
        update_cond[br] = update_info.isCond[br] &&
                          !update_info.isAlwaysTaken[br];
    }
    /** squash recover */
    //tage ghr recover
    if (squash_info.is_squash){
        ittage_blk->squash_recover(squash_info.tid,
                                 squash_info.actual_taken,
                                 squash_info.tage_sc_i_bInfo.ittageBranchInfo,
                                 squash_info.isCond && squash_info.isMispred,
                                 squash_info.numBrBefore);
        if (enableSC){
            new_sc_blk->squash(squash_info.tid, squash_info.actual_taken,
                            squash_info.startAddr,
                            squash_info.tage_sc_i_bInfo.newSCBranchInfo,
                            squash_info.isCond && squash_info.isMispred,
                            squash_info.numBrBefore,
                            &squash_info.condTarget[0]);
        }
        if (enableSLoopPred){
            loop_pred_blk->squash(squash_info.startAddr, squash_info.offset,
                            squash_info.tid, squash_info.actual_taken,
                            squash_info.tage_sc_i_bInfo.loopPredBranchInfo,
                            squash_info.isCond && squash_info.isMispred,
                            squash_info.numBrBefore, squash_info.ftqIdx);
        }
    }else{
        if (pred_enable){
            bool use_tage_ctr[MaxNumBr];
            int8_t tage_ctr[MaxNumBr];
            bool bias[MaxNumBr];
            Addr condTarget[MaxNumBr];
            bool always_taken[MaxNumBr];
            int8_t cond_offset[MaxNumBr];
            for (unsigned br = 0; br < MaxNumBr; ++br){
                cond_offset[br] = tage_sc_i_bInfo.tagebtbBranchInfo->
                            btbBranchInfo->hitBtbEntry.slot[br].offset;
            }
            if (enableSC){
                BTB_TAGE::TageBranchInfo *bi_tage =
                            static_cast<BTB_TAGE::TageBranchInfo*>(
                            tage_sc_i_bInfo.tagebtbBranchInfo->
                            dirBranchInfo);
                for (unsigned br = 0; br < MaxNumBr; ++br){
                    use_tage_ctr[br] =
                        //bi_tage->tageBranchInfo->hitBank[br] > 0;
                        bi_tage->tageBranchInfo->provider[br] == 1;
                    tage_ctr[br] = use_tage_ctr[br] ?
                                bi_tage->tageBranchInfo->hitBankCtr[br] :
                                0;
                    bias[br] = (bi_tage->tageBranchInfo->longestMatchPred[br]
                            != bi_tage->tageBranchInfo->altTaken[br]);
                    condTarget[br] = tage_sc_i_bInfo.tagebtbBranchInfo->
                                    btbBranchInfo->hitBtbEntry.slot[br].target;
                    always_taken[br] = tage_sc_i_bInfo.tagebtbBranchInfo->
                            btbBranchInfo->hitBtbEntry.slot[br].always_taken;
                }
                new_sc_blk->scPredict(tid, startAddr, pred_isCond,
                     &always_taken[0], tage_sc_i_bInfo.newSCBranchInfo,
                     pred_cond_taken, &bias[0],
                     &tage_ctr[0], btb_tage->getTageCtrBits(),
                     bi_tage->tageBranchInfo->hitBank,
                     bi_tage->tageBranchInfo->altBank,
                     &condTarget[0], confidence);
            }

            if (enableSLoopPred){
                loop_pred_blk->loopPredict(tid, startAddr,
                            pred_isCond, &cond_offset[0], pred_cond_taken,
                            tage_sc_i_bInfo.loopPredBranchInfo, 1,
                            ftqIdx, confidence);
            }

            bool taken_is_indir;
            bool taken_is_ret;
            DPRINTF(TAGE_SC_I_BLK,
                    "target before s3=%x, pred_isCond[0]=%d, \
                    pred_cond_taken[0]=%d\n",
                    target, pred_isCond[0], pred_cond_taken[0]);
            if (!pred_isCond[0] || !pred_cond_taken[0]){
                taken_is_indir = slot1_is_indir;
                taken_is_ret = slot1_is_ret;
                if (tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->btb_hit &&
                    tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[1].valid){
                    if (pred_isCond[1]){
                        pred_taken = pred_cond_taken[1];
                    }else{
                        pred_taken = true;
                    }
                    if (pred_taken){
                        pred_offset = tage_sc_i_bInfo.tagebtbBranchInfo->
                                    btbBranchInfo->hitBtbEntry.slot[1].offset;
                    }else{
                        pred_offset = 0;
                    }
                    target = tage_sc_i_bInfo.tagebtbBranchInfo->
                                    btbBranchInfo->hitBtbEntry.slot[1].target;
                }else{
                    pred_taken = false;
                    pred_offset = 0;
                    target = startAddr + MaxInstrsPerBlock*16;
                }
            }else{
                taken_is_indir = false;
                taken_is_ret = false;
                pred_taken = true;
                pred_offset = tage_sc_i_bInfo.tagebtbBranchInfo->
                                    btbBranchInfo->hitBtbEntry.slot[0].offset;
                target = tage_sc_i_bInfo.tagebtbBranchInfo->
                                    btbBranchInfo->hitBtbEntry.slot[0].target;
            }
            if (taken_is_ret){
                // target = tage_sc_i_bInfo.rasmeta->
                //         tos[tage_sc_i_bInfo.rasmeta->sp].retAddr;
                target = ras_blk->getRetAddr(tage_sc_i_bInfo.rasmeta);
            }
            //ittage
            Addr indir_target = ittage_blk->predict(tid, startAddr,
                                    pred_cond_taken,
                                    pred_isCond,
                                    tage_sc_i_bInfo.ittageBranchInfo,
                                    target, confidence);

            if (taken_is_indir && !taken_is_ret){
                target = indir_target;
            }
            DPRINTF(TAGE_SC_I_BLK,
                    "taken_is_indir=%d, taken_is_ret=%d, \
                    target after s3=%x\n",
                    taken_is_indir, taken_is_ret,
                    target);

            //spec update historys
            if (enableSC){
                new_sc_blk->specUpdateHist(startAddr, pred_isCond,
                                pred_cond_taken, &condTarget[0]);
            }

            if (enableSLoopPred){
                loop_pred_blk->specUpdateHist(tid, startAddr,
                            pred_isCond, &cond_offset[0],
                            pred_cond_taken,
                            tage_sc_i_bInfo.loopPredBranchInfo);
            }

            ittage_blk->updateHistories(tid, startAddr, pred_cond_taken,
                    pred_isCond, tage_sc_i_bInfo.ittageBranchInfo, true);

        }
    }

    /** commit */
    if (update_info.valid){
        BTB_TAGE::TageBranchInfo *bi_tage_update =
                            static_cast<BTB_TAGE::TageBranchInfo*>
                            (update_info.tage_sc_i_bInfo.
                            tagebtbBranchInfo->dirBranchInfo);
        if (enableSC){
            bool bias_update[MaxNumBr];
            for (unsigned br = 0; br < MaxNumBr; ++br){
                bias_update[br] =
                    (bi_tage_update->tageBranchInfo->longestMatchPred[br] !=
                    bi_tage_update->tageBranchInfo->altTaken[br]);
            }
            new_sc_blk->condBranchUpdate(update_info.tid,
                                update_info.startAddr,
                                &update_cond[0],
                                &update_info.actual_taken[0],
                                update_info.tage_sc_i_bInfo.newSCBranchInfo,
                                &bias_update[0],
                                bi_tage_update->tageBranchInfo->hitBank,
                                bi_tage_update->tageBranchInfo->altBank);
        }
        if (enableSLoopPred){
            bool update_prev_pred_taken[MaxNumBr];
            int8_t update_cond_offset[MaxNumBr];
            if (enableSC){
                for (unsigned br = 0; br < MaxNumBr; ++br){
                    update_prev_pred_taken[br] =
                        update_info.tage_sc_i_bInfo.
                        newSCBranchInfo->scPred[br];
                }
            }else{
                for (unsigned br = 0; br < MaxNumBr; ++br){
                    update_prev_pred_taken[br] =
                            bi_tage_update->tageBranchInfo->
                            tagePred[br] || (update_info.tage_sc_i_bInfo.
                            tagebtbBranchInfo->btbBranchInfo->btb_hit &&
                            update_info.tage_sc_i_bInfo.
                            tagebtbBranchInfo->btbBranchInfo->
                            hitBtbEntry.slot[br].valid &&
                            update_info.tage_sc_i_bInfo.
                            tagebtbBranchInfo->btbBranchInfo->
                            hitBtbEntry.slot[br].always_taken);
                }
            }
            for (unsigned br = 0; br < MaxNumBr; ++br){
                update_cond_offset[br] =
                            update_info.new_btb_entry.slot[br].offset;
            }
            loop_pred_blk->condBranchUpdate(update_info.tid,
                                            update_info.startAddr,
                                            update_info.ftqIdx,
                                            &update_info.isCond[0],
                                            &update_cond_offset[0],
                                            &update_info.actual_taken[0],
                                            &update_prev_pred_taken[0],
                            update_info.tage_sc_i_bInfo.loopPredBranchInfo,
                                            1);
        }
        ittage_blk->update(update_info.tid, update_info.startAddr,
                        update_info.indir_target, update_info.isIndir,
                        update_info.tage_sc_i_bInfo.ittageBranchInfo, press);
    }

    /*free mem*/
    if (enableSC){
        if (free_tage_sc_i_bInfos.size() > 0){
            for (auto& free_entry : free_tage_sc_i_bInfos){
                new_sc_blk->free_mem(free_entry.newSCBranchInfo);
            }
        }
    }

    if (enableSLoopPred){
        if (free_tage_sc_i_bInfos.size() > 0){
            for (auto& free_entry : free_tage_sc_i_bInfos){
                loop_pred_blk->free_mem(free_entry.loopPredBranchInfo);
            }
        }
    }

    if (free_tage_sc_i_bInfos.size() > 0){
        for (auto& free_entry : free_tage_sc_i_bInfos){
            ittage_blk->free_mem(free_entry.ittageBranchInfo);
        }
    }
}

} // namespace branch_prediction
} // namespace gem5

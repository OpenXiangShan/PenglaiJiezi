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
 * Implementation of a TAGE branch predictor. TAGE is a global-history based
 * branch predictor. It features a PC-indexed bimodal predictor and N
 * partially tagged tables, indexed with a hash of the PC and the global
 * branch history. The different lengths of global branch history used to
 * index the partially tagged tables grow geometrically. A small path history
 * is also used in the hash.
 *
 * All TAGE tables are accessed in parallel, and the one using the longest
 * history that matches provides the prediction (some exceptions apply).
 * Entries are allocated in components using a longer history than the
 * one that predicted when the prediction is incorrect.
 */

#ifndef __CPU_PRED_TAGE_SC_I_BLK_HH__
#define __CPU_PRED_TAGE_SC_I_BLK_HH__

#include <vector>

#include "base/types.hh"
#include "cpu/pred/dppred_blk/btb_blk.hh"
#include "cpu/pred/dppred_blk/btb_dir.hh"
#include "cpu/pred/dppred_blk/btb_tage.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "cpu/pred/dppred_blk/ittage_blk.hh"
#include "cpu/pred/dppred_blk/loop_pred_blk.hh"
#include "cpu/pred/dppred_blk/new_sc_blk.hh"
#include "cpu/pred/dppred_blk/ras_lk_blk.hh"
#include "cpu/pred/dppred_blk/ubtb_blk.hh"
#include "params/TAGE_SC_I_BLK.hh"

namespace gem5
{

namespace branch_prediction
{

class TAGE_SC_I_BLK: public SimObject
{
  public:
    TAGE_SC_I_BLK(const TAGE_SC_I_BLKParams &params);
    struct TageSCIBranchInfo
    {
        BTB_DIR::DIRBTBBranchInfo *tagebtbBranchInfo;
        RAS_BLK::Meta *rasmeta;
        ITTAGE_BLK::ITTageBranchInfo *ittageBranchInfo;
        UBTB_BLK::UBTBBranchInfo *ubtbBranchInfo;
        New_SC_BLK::BranchInfo *newSCBranchInfo;
        LOOP_PRED_BLK::BranchInfo *loopPredBranchInfo;
    };

  public:
    bool enableSC;
    bool enableSLoopPred;
    UBTB_BLK *ubtb_blk;
    BTB_TAGE *btb_tage;
    RAS_BLK *ras_blk;
    ITTAGE_BLK *ittage_blk;
    New_SC_BLK *new_sc_blk;
    LOOP_PRED_BLK *loop_pred_blk;

    struct SquashInfo
    {
      bool is_squash;
      unsigned ftqIdx;
      ThreadID tid;
      TageSCIBranchInfo tage_sc_i_bInfo;
      bool actual_taken;
      bool isCond;
      bool isCall;
      Addr callPushAddr;
      bool isRet;
      bool isMispred;
      //number of cond Br before flush inst in blk
      unsigned numBrBefore;
      Addr startAddr;
      int8_t offset;
      Addr condTarget[MaxNumBr];
    };

    struct UpdateInfo
    {
      unsigned ftqIdx;
      bool valid;
      Addr startAddr;
      ThreadID tid;
      bool actual_taken[MaxNumBr];
      bool isCond[MaxNumBr];
      bool isAlwaysTaken[MaxNumBr];
      bool isIndir; //first taken branch is indir
      bool isCall;
      bool isRet;
      Addr indir_target;
      TageSCIBranchInfo tage_sc_i_bInfo;
      BTBEntry new_btb_entry;
      bool allocate_btb;
      bool update_btb;
      bool false_hit;
      bool slot0_false_empty;
    };


    Addr getRetAddr(TageSCIBranchInfo tage_sc_i_bInfo);

    bool btb_alloc_update_lookup(Addr startAddr,
                    ThreadID tid, BTBEntry &hit_btb_entry);

    void tick_s1(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool *confidence);

    void tick_s2(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool &slot1_is_indir,
                        bool &slot1_is_call,
                        bool &slot1_is_ret, bool *pred_isCond,
                        bool *pred_cond_taken, bool *confidence);

    void tick_s3(SquashInfo squash_info, UpdateInfo update_info,
                        TageSCIBranchInfo &tage_sc_i_bInfo, Addr &startAddr,
                        Addr &endAddr, Addr &target, bool &pred_taken,
                        unsigned &pred_offset, ThreadID &tid,
                        std::vector<TageSCIBranchInfo> free_tage_sc_i_bInfos,
                        bool pred_enable, bool slot1_is_indir,
                        bool slot1_is_ret, bool *pred_isCond,
                        bool *pred_cond_taken, bool &press,
                        unsigned ftqIdx, bool *confidence);
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_TAGE_SC_I_BLK_HH__

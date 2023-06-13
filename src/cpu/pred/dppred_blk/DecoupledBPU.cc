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

#include "cpu/pred/dppred_blk/DecoupledBPU.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "debug/DecoupledBPU.hh"
#include "debug/Fetch.hh"
#include "debug/Tage.hh"

namespace gem5
{

namespace branch_prediction
{
DecoupledBPU::DecoupledBPU(const DecoupledBPUParams &params):
            SimObject(params), tage_sc_i_blk(params.tage_sc_i_blk),
            ftq(params.ftq), S1Queue(10,10),
            S2Queue(10,10), stats(this){

    ftq_entry_pred_s1.startAddr = 0;   //reset core start addr
    ftq_entry_pred_s1.pred_taken = false;
    interrupt_pending = false;
    toS2 = S1Queue.getWire(0);
    fromS1 = S1Queue.getWire(-1);
    toS3 = S2Queue.getWire(0);
    fromS2 = S2Queue.getWire(-1);
}

DecoupledBPU::DecoupledBPUStats::DecoupledBPUStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(condCommitted, statistics::units::Count::get(),
               "Number of conditional branches committed"),
      ADD_STAT(indirCommitted, statistics::units::Count::get(),
               "Number of indir branches committed"),
      ADD_STAT(retCommitted, statistics::units::Count::get(),
               "Number of ret branches committed"),
      ADD_STAT(condIncorrect, statistics::units::Count::get(),
               "Number of conditional branches incorrect"),
      ADD_STAT(indirIncorrect, statistics::units::Count::get(),
               "Number of indir branches incorrect"),
      ADD_STAT(retIncorrect, statistics::units::Count::get(),
               "Number of ret branches incorrect"),
      ADD_STAT(ubtbCommitHit, statistics::units::Count::get(),
               "Number of uBTB commit hit"),
      ADD_STAT(ubtbCommitMiss, statistics::units::Count::get(),
               "Number of uBTB commit miss"),
      ADD_STAT(ubtbLookups, statistics::units::Count::get(),
               "Number of uBTB lookups"),
      ADD_STAT(s2_redir, statistics::units::Count::get(),
               "Number of s2 redirect"),
      ADD_STAT(s3_redir, statistics::units::Count::get(),
               "Number of s3 redirect"),
      ADD_STAT(condIncorrect_for_btb_miss, statistics::units::Count::get(),
               "Number of condIncorrect for btb miss"),
      ADD_STAT(condIncorrect_for_sfb, statistics::units::Count::get(),
               "Number of condIncorrect for short forward branches"),
      ADD_STAT(confHighMisp, statistics::units::Count::get(),
               "Number of confidence high misp"),
      ADD_STAT(confLowMisp, statistics::units::Count::get(),
               "Number of confidence low misp"),
      ADD_STAT(confHighRight, statistics::units::Count::get(),
               "Number of confidence high right"),
      ADD_STAT(confLowRight, statistics::units::Count::get(),
               "Number of confidence low right"),
      ADD_STAT(BTBLookups, statistics::units::Count::get(),
               "Number of BTB lookups"),
      ADD_STAT(BTBHits, statistics::units::Count::get(), "Number of BTB hits"),
      ADD_STAT(updateNum, statistics::units::Count::get(),
                            "Number of hit or taken"),
      ADD_STAT(BTBHitRatio, statistics::units::Ratio::get(), "BTB Hit Ratio",
               BTBHits / BTBLookups)
{
    BTBHitRatio.precision(6);
}

void
DecoupledBPU::setStartAddr(Addr startAddr)
{
    ftq_entry_pred_s1.startAddr = startAddr; //reset core start addr
    ftq_entry_pred_s1.pred_taken = false;
}

Addr
DecoupledBPU::getRetAddr(unsigned ftq_idx)  //get ras ret addr
{
    FTQ::FtqEntry ftq_entry = ftq->get_ftq_entry(ftq_idx);
    return tage_sc_i_blk->getRetAddr(ftq_entry.tage_sc_i_bInfo);
}

void
DecoupledBPU::setBpuToFetchQueue(TimeBuffer<BpuToFetchStruct> *time_buffer)
{
    // Create wire to write information to proper place in fetch time buf.
    toFetch = time_buffer->getWire(0);
}

void
DecoupledBPU::setFetchToBpuQueue(TimeBuffer<FetchToBpuStruct> *time_buffer)
{
    // Create wires to get information from proper places in time buffer.
    fromFetch = time_buffer->getWire(-1);
}

BTBBranchInfo
DecoupledBPU::get_ftb_for_precheck(unsigned ftq_idx){
    FTQ::FtqEntry ftq_entry = ftq->get_ftq_entry(ftq_idx);
    return *(ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->btbBranchInfo);
}

unsigned
DecoupledBPU::new_btb_gen(Addr start_addr, BTBEntry old_btb_entry,
                          PdInfo pdInfo, FTQ::CfiInfo cfi_info,
                          Addr target, bool hit,
                          bool mispred[MaxInstrsPerBlock],
                          BTBEntry &new_btb_entry,
                          bool &slot0_false_empty){

    bool btb_allocate = false;
    bool btb_modify = false;
    slot0_false_empty = false;

    assert(pdInfo.validInstr[cfi_info.cfiIndex] || !cfi_info.valid);

    bool new_jmp_is_jal = pdInfo.hasJmp &&
                        pdInfo.isDirect && cfi_info.valid;
    bool new_jmp_is_jalr = pdInfo.hasJmp &&  //here mret is regarded as indir
                        !pdInfo.isDirect && cfi_info.valid;
    bool new_jmp_is_call = pdInfo.hasJmp &&
                        pdInfo.jmpAttr == JmpType::CALL && cfi_info.valid;
    //should exclude mret
    bool new_jmp_is_ret = pdInfo.hasJmp &&
                        pdInfo.jmpAttr == JmpType::RET && cfi_info.valid;

    //first taken is br in block
    bool cfi_is_br = pdInfo.brMask[cfi_info.cfiIndex] && cfi_info.valid;
    bool cfi_is_jalr = pdInfo.jmpOffset == cfi_info.cfiIndex &&
                        new_jmp_is_jalr;
    DPRINTF(DecoupledBPU,
            "ftb allocate or update startAddr: %x, index:%d, tag:%d\n",
            start_addr, tage_sc_i_blk->btb_tage->BTB.getIndex(start_addr, 0),
            tage_sc_i_blk->btb_tage->BTB.getTag(start_addr));

            DPRINTF(DecoupledBPU,
                    "old btb entry, valid=%d\n", old_btb_entry.valid);
            DPRINTF(DecoupledBPU,
                    "old btb entry, slot0_valid=%d, \
                    slot0_offset=%d, slot0_target=%x\n",
                    old_btb_entry.slot[0].valid,
                    old_btb_entry.slot[0].offset,
                    old_btb_entry.slot[0].target);

            DPRINTF(DecoupledBPU,
                    "old btb entry, slot1_valid=%d, \
                    slot1_offset=%d, slot1_target=%x, slot1_sharing=%d\n",
                    old_btb_entry.slot[1].valid,
                    old_btb_entry.slot[1].offset,
                    old_btb_entry.slot[1].target,
                    old_btb_entry.slot[1].sharing);
    if (!hit){
        if (cfi_is_br){
            new_btb_entry.slot[0].valid = true;
            new_btb_entry.slot[0].offset = cfi_info.cfiIndex;
            new_btb_entry.slot[0].sharing = false;
            new_btb_entry.slot[0].target = target;
            new_btb_entry.slot[0].always_taken = true;
        }
        if (pdInfo.hasJmp){
            new_btb_entry.slot[1].valid = true;
            assert(new_jmp_is_jal || new_jmp_is_jalr ||
                    new_jmp_is_call || new_jmp_is_ret);
            new_btb_entry.slot[1].offset = pdInfo.jmpOffset;
            new_btb_entry.slot[1].sharing = false;
            new_btb_entry.slot[1].always_taken = false;
            DPRINTF(DecoupledBPU, "jmpAttr: %d, jmpOffset:%d, isDir:%d\n",
                    pdInfo.jmpAttr, pdInfo.jmpOffset, pdInfo.isDirect);
            DPRINTF(DecoupledBPU, "cfi valid: %d, cfi index:%d\n",
                    cfi_info.valid, cfi_info.cfiIndex);
            if (cfi_is_jalr){
                new_btb_entry.slot[1].target = target;
            }else{
                new_btb_entry.slot[1].target = pdInfo.jmpTarget;
            }
        }
        new_btb_entry.tag = tage_sc_i_blk->btb_tage->BTB.getTag(start_addr);
        new_btb_entry.tid = 0;
        new_btb_entry.isJalr = new_jmp_is_jalr;
        new_btb_entry.isCall = new_jmp_is_call;
        new_btb_entry.isRet = new_jmp_is_ret;
        if (pdInfo.hasJmp){
            if (pdInfo.rvcMask[pdInfo.jmpOffset]){
                new_btb_entry.fallThruAddr = start_addr +
                                                pdInfo.jmpOffset*2 + 2;
            }else{
                new_btb_entry.fallThruAddr = start_addr +
                                                pdInfo.jmpOffset*2 + 4;
            }
        }else{
            new_btb_entry.fallThruAddr = start_addr + MaxInstrsPerBlock*2;
        }

        new_btb_entry.valid = true;
        btb_allocate = true;
        DPRINTF(DecoupledBPU, "allocate btb\n");
        DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                    new_btb_entry.fallThruAddr);
    }else{
        if (!old_btb_entry.slot[0].valid){
            slot0_false_empty = true;
        }
        new_btb_entry = old_btb_entry;
        if (cfi_info.valid &&
            old_btb_entry.slot[0].valid &&
            cfi_info.cfiIndex < old_btb_entry.slot[0].offset){
            //insert to slot0
            new_btb_entry.slot[0].valid = true;
            new_btb_entry.slot[0].offset = cfi_info.cfiIndex;
            new_btb_entry.slot[0].sharing = false;
            new_btb_entry.slot[0].target = target;
            new_btb_entry.slot[0].always_taken = true;
            new_btb_entry.slot[1] = old_btb_entry.slot[0];
            new_btb_entry.slot[1].sharing = true;
            if (old_btb_entry.slot[1].valid){
                new_btb_entry.fallThruAddr = start_addr +
                                        old_btb_entry.slot[1].offset*2;
            }else{
                new_btb_entry.fallThruAddr = start_addr + MaxInstrsPerBlock*2;
            }
            btb_modify = true;
            DPRINTF(DecoupledBPU, "insert to slot0\n");
            DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                        new_btb_entry.fallThruAddr);
        }else if (!old_btb_entry.slot[0].valid &&
                 cfi_info.valid &&
                 old_btb_entry.slot[1].valid &&
                 cfi_info.cfiIndex < old_btb_entry.slot[1].offset
                 ){
            assert(!old_btb_entry.slot[1].sharing);
            //write to slot0
            new_btb_entry.slot[0].valid = true;
            new_btb_entry.slot[0].offset = cfi_info.cfiIndex;
            new_btb_entry.slot[0].sharing = false;
            new_btb_entry.slot[0].target = target;
            new_btb_entry.slot[0].always_taken = true;
            btb_modify = true;
            DPRINTF(DecoupledBPU, "write to slot0\n");
            DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                        new_btb_entry.fallThruAddr);
        }else if (cfi_info.valid &&
                old_btb_entry.slot[0].valid &&
                cfi_info.cfiIndex > old_btb_entry.slot[0].offset &&
                old_btb_entry.slot[1].valid &&
                cfi_info.cfiIndex < old_btb_entry.slot[1].offset){
            //insert to slot1
            new_btb_entry.slot[0].always_taken = false;
            new_btb_entry.slot[1].valid = true;
            new_btb_entry.slot[1].offset = cfi_info.cfiIndex;
            new_btb_entry.slot[1].sharing = true;
            new_btb_entry.slot[1].target = target;
            new_btb_entry.slot[1].always_taken = true;
            new_btb_entry.tid = 0;
            new_btb_entry.isCall = false;
            new_btb_entry.isRet = false;
            new_btb_entry.isJalr = false;
            new_btb_entry.tag =
                tage_sc_i_blk->btb_tage->BTB.getTag(start_addr);
            new_btb_entry.fallThruAddr = start_addr +
                                        old_btb_entry.slot[1].offset*2;
            new_btb_entry.valid = true;
            btb_modify = true;
            assert((old_btb_entry.slot[1].valid &&
                    tage_sc_i_blk->btb_tage->BTB.getTag(start_addr) ==
                    old_btb_entry.tag));
            DPRINTF(DecoupledBPU, "insert to slot1\n");
            DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                        new_btb_entry.fallThruAddr);
        }else if (cfi_info.valid &&
                 old_btb_entry.slot[0].valid &&
                 cfi_info.cfiIndex > old_btb_entry.slot[0].offset &&
                 !old_btb_entry.slot[1].valid){
            //write to slot1
            new_btb_entry.slot[0].always_taken = false;
            new_btb_entry.slot[1].valid = true;
            new_btb_entry.slot[1].offset = cfi_info.cfiIndex;
            new_btb_entry.slot[1].sharing = true;
            new_btb_entry.slot[1].target = target;
            new_btb_entry.slot[1].always_taken = true;
            new_btb_entry.tid = 0;
            new_btb_entry.isCall = false;
            new_btb_entry.isRet = false;
            new_btb_entry.isJalr = false;
            new_btb_entry.tag =
                tage_sc_i_blk->btb_tage->BTB.getTag(start_addr);
            new_btb_entry.fallThruAddr = start_addr +
                                            MaxInstrsPerBlock*2;
            new_btb_entry.valid = true;
            btb_modify = true;
            DPRINTF(DecoupledBPU, "write to slot1\n");
            DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                        new_btb_entry.fallThruAddr);
        }else if (cfi_info.valid &&
                    old_btb_entry.slot[1].valid &&
                    cfi_info.cfiIndex > old_btb_entry.slot[1].offset &&
                    old_btb_entry.slot[0].valid){
            new_btb_entry.slot[0].always_taken = false;
            new_btb_entry.slot[1].always_taken = false;
            new_btb_entry.fallThruAddr = start_addr + cfi_info.cfiIndex*2;
            btb_modify = true;
            assert(old_btb_entry.slot[1].sharing);
            DPRINTF(DecoupledBPU, "no insert\n");
            DPRINTF(DecoupledBPU, "fallThrough Addr is: %x\n",
                        new_btb_entry.fallThruAddr);
        }

        //modify jalr target
        if (cfi_is_jalr &&
            old_btb_entry.slot[1].valid &&
            !old_btb_entry.slot[1].sharing &&
            old_btb_entry.slot[1].target != target){
            new_btb_entry.slot[1].target = target;
            btb_modify = true;
            DPRINTF(DecoupledBPU, "modify jalr target\n");
            assert(old_btb_entry.slot[1].valid);

        }

        //modify always_taken
        for (unsigned i = 0; i <= MaxNumBr-1; ++i){
            if (!(cfi_info.valid &&
                old_btb_entry.slot[i].valid &&
                cfi_info.cfiIndex == old_btb_entry.slot[i].offset) &&
                (i==0 || old_btb_entry.slot[i].sharing) &&
                old_btb_entry.slot[i].always_taken){
                new_btb_entry.slot[i].always_taken = false;
                btb_modify = true;
                DPRINTF(DecoupledBPU, "modify always_taken\n");
            }
        }
    }
    if (new_btb_entry.slot[0].valid && new_btb_entry.slot[1].valid){
        assert(new_btb_entry.slot[0].offset < new_btb_entry.slot[1].offset);
    }

    DPRINTF(DecoupledBPU,
            "new btb entry, valid=%d\n", new_btb_entry.valid);
    DPRINTF(DecoupledBPU,
            "new btb entry, slot0_valid=%d, \
            slot0_offset=%d, slot0_target=%x\n",
            new_btb_entry.slot[0].valid,
            new_btb_entry.slot[0].offset,
            new_btb_entry.slot[0].target);

    DPRINTF(DecoupledBPU,
            "new btb entry, slot1_valid=%d, \
            slot1_offset=%d, slot1_target=%x, slot1_sharing=%d\n",
            new_btb_entry.slot[1].valid,
            new_btb_entry.slot[1].offset,
            new_btb_entry.slot[1].target,
            new_btb_entry.slot[1].sharing);
    if (new_btb_entry.slot[0].valid && new_btb_entry.slot[1].valid &&
        new_btb_entry.valid) {
        assert(new_btb_entry.slot[0].offset < new_btb_entry.slot[1].offset);
    }

    if (btb_allocate){
        return 1;
    } else if (btb_modify){
        return 2;
    }else{
        return 0;
    }

}


void
DecoupledBPU::tick(){
    FTQ::CfiInfo cfi_info;
    FTQ::FtqEntry ftq_entry;
    unsigned instr_offset;
        /* create mem free entries*/
    std::vector<FTQ::FtqEntry> free_s1_ftq_entries;
    std::vector<TAGE_SC_I_BLK::TageSCIBranchInfo> free_s1_tage_sc_i_bInfos;
    std::vector<FTQ::FtqEntry> free_s2_ftq_entries;
    std::vector<TAGE_SC_I_BLK::TageSCIBranchInfo> free_s2_tage_sc_i_bInfos;
    std::vector<FTQ::FtqEntry> free_s3_ftq_entries;
    std::vector<TAGE_SC_I_BLK::TageSCIBranchInfo> free_s3_tage_sc_i_bInfos;

    /******** squash ********/
    if (fromFetch->backendSquash.squash){  //backend squash
        squash_info.tid = 0;
        //update commitStatus
        if (fromFetch->backendSquash.squashInst){
            DPRINTF(DecoupledBPU, "Backend squash at pc: %#x\n",
                    fromFetch->backendSquash.squashInst->
                            pcState().instAddr());
            DPRINTF(DecoupledBPU, "Backend squash target: %#x\n",
                    fromFetch->backendSquash.pc->instAddr());
            DPRINTF(DecoupledBPU, "Backend squash at ftqIdx: %d\n",
                    fromFetch->backendSquash.squashInst->ftqIdx);
            DPRINTF(DecoupledBPU, "Backend squash at seqNum: %d\n",
                    fromFetch->backendSquash.squashInst->seqNum);
            DPRINTF(DecoupledBPU, "Backend squash itself: %d\n",
                    fromFetch->backendSquash.squashItSelf);
            if (fromFetch->backendSquash.mispredictInst){
                DPRINTF(DecoupledBPU, "Backend squash because of misp\n");
                DPRINTF(DecoupledBPU, "Backend squash taken: %d\n",
                    fromFetch->backendSquash.branchTaken);
            }else{
                DPRINTF(DecoupledBPU, "Backend squash because of non-misp\n");
            }
            ftq_entry = ftq->get_ftq_entry(fromFetch->
                            backendSquash.squashInst->ftqIdx);
            DPRINTF(DecoupledBPU, "Backend squash startAddr is: %#x\n",
                        ftq_entry.startAddr);

            instr_offset = (fromFetch->backendSquash.squashInst->
                            pcState().instAddr() - ftq_entry.startAddr) >> 1;
            assert(instr_offset < 16);
            //assert should consider interrupt suqash is after instr commit
            assert(ftq_entry.commit_status[instr_offset] ==
                        FTQ::FtqCommitState::VALID ||
                        ftq_entry.commit_status[instr_offset] ==
                        FTQ::FtqCommitState::COMMITED);
            if (!fromFetch->backendSquash.squashItSelf){
                for (unsigned i = instr_offset+1;
                                    i <= MaxInstrsPerBlock-1; ++i){
                    ftq->write_commitstatus_to_ftq(
                                fromFetch->backendSquash.squashInst->ftqIdx,
                                FTQ::FtqCommitState::INVALID,
                                i);
                }
            }else{
                for (unsigned i = instr_offset; i <= MaxInstrsPerBlock-1; ++i){
                    ftq->write_commitstatus_to_ftq(
                                fromFetch->backendSquash.squashInst->ftqIdx,
                                FTQ::FtqCommitState::INVALID,
                                i);
                }
            }
        }

        //branch mispred squash
        if (fromFetch->backendSquash.mispredictInst){
            assert(!fromFetch->backendSquash.squashItSelf);
            assert(fromFetch->backendSquash.mispredictInst->isControl());
            //for mret, isIndirectCtrl=false; isReturn=true
            assert(fromFetch->backendSquash.mispredictInst->isCondCtrl() ||
                fromFetch->backendSquash.mispredictInst->isIndirectCtrl() ||
                fromFetch->backendSquash.mispredictInst->isReturn() ||
                fromFetch->backendSquash.mispredictInst->isCall());
            trace_confidence = true;
            squash_info.is_squash = true;
            squash_info.isCond =
                    fromFetch->backendSquash.mispredictInst->isCondCtrl();
            squash_info.isCall =
                    fromFetch->backendSquash.mispredictInst->isCall();
            squash_info.isRet =
                    fromFetch->backendSquash.mispredictInst->isReturn() &&
                    fromFetch->backendSquash.mispredictInst->isIndirectCtrl();
            if (squash_info.isCall){
                if (fromFetch->backendSquash.mispredictInst->compressed()){
                    squash_info.callPushAddr =
                                fromFetch->backendSquash.mispredictInst->
                                pcState().instAddr() + 2;
                }else{
                    squash_info.callPushAddr =
                                fromFetch->backendSquash.mispredictInst->
                                pcState().instAddr() + 4;
                }
            }else{
                squash_info.callPushAddr = 0;
            }
            ftq_entry =
                ftq->get_ftq_entry(fromFetch->
                backendSquash.mispredictInst->ftqIdx);
            squash_info.startAddr = ftq_entry.startAddr;
            squash_info.ftqIdx = ftq_entry.ftq_idx;
            instr_offset = (fromFetch->backendSquash.mispredictInst->
                        pcState().instAddr() -
                        ftq_entry.startAddr) >> 1;
            squash_info.offset = instr_offset;
            squash_info.tage_sc_i_bInfo =
                                    ftq_entry.tage_sc_i_bInfo;
            squash_info.isMispred = true;
            squash_info.actual_taken = fromFetch->backendSquash.branchTaken;
            squash_info.numBrBefore = 0;
            squash_info.condTarget[MaxNumBr-1] =
                                fromFetch->backendSquash.pc->instAddr();
            for (unsigned i = 0; i < MaxNumBr-1; i++){
                squash_info.condTarget[i] =
                                fromFetch->backendSquash.pc->instAddr();
                if (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->btb_hit &&
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo
                        ->btbBranchInfo->hitBtbEntry.slot[i].valid &&
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->hitBtbEntry.slot[i].offset <
                        instr_offset){
                    squash_info.numBrBefore++;
                    squash_info.condTarget[i] = ftq_entry.tage_sc_i_bInfo.
                                        tagebtbBranchInfo->btbBranchInfo->
                                        hitBtbEntry.slot[i].target;
                }
            }

            /** update cfiindex in ftq */
            cfi_info.cfiIndex =
                (fromFetch->backendSquash.mispredictInst->pcState().instAddr()
                    - ftq_entry.startAddr) >> 1;

            if ((!ftq_entry.cfiIndex.valid ||
                cfi_info.cfiIndex < ftq_entry.cfiIndex.cfiIndex) &&
                fromFetch->backendSquash.branchTaken &&
                fromFetch->backendSquash.mispredictInst->isControl()){
                    cfi_info.valid = true;
                    ftq->write_cfiindex_to_ftq(fromFetch->
                        backendSquash.mispredictInst->ftqIdx, cfi_info);
                DPRINTF(DecoupledBPU,
                "write cfiindex backendSquash ftqIdx: %d, branchAddr: %x; "
                "startAddr: %x, cfi_info.valid:%d, cfi_info.cfiIndex:%d\n",
                fromFetch->backendSquash.mispredictInst->ftqIdx,
                fromFetch->backendSquash.mispredictInst->pcState().instAddr(),
                ftq_entry.startAddr, cfi_info.valid, cfi_info.cfiIndex);
            }else if (ftq_entry.cfiIndex.valid &&
                cfi_info.cfiIndex == ftq_entry.cfiIndex.cfiIndex &&
                !fromFetch->backendSquash.branchTaken &&
                fromFetch->backendSquash.mispredictInst->isControl()){
                    cfi_info.valid = false;
                    ftq->write_cfiindex_to_ftq(fromFetch->
                        backendSquash.mispredictInst->ftqIdx, cfi_info);
                DPRINTF(DecoupledBPU,
                "write cfiindex backendSquash ftqIdx: %d, branchAddr: %x; "
                "startAddr: %x, cfi_info.valid:%d, cfi_info.cfiIndex:%d\n",
                fromFetch->backendSquash.mispredictInst->ftqIdx,
                fromFetch->backendSquash.mispredictInst->pcState().instAddr(),
                ftq_entry.startAddr, cfi_info.valid, cfi_info.cfiIndex);
            }

            /** update mispred in ftq */
            ftq->write_mispred_to_ftq(fromFetch->
                backendSquash.mispredictInst->ftqIdx, true,
                fromFetch->backendSquash.mispredictInst->pcState().instAddr());
            /** update target in ftq */
            ftq->write_target_to_ftq(fromFetch->
                        backendSquash.mispredictInst->ftqIdx,
                        fromFetch->backendSquash.pc->instAddr());

            //free mem
            // DPRINTF(DecoupledBPU,
            //     "squash ftqIdx: %d, pred_idx: %d\n",
            //     fromFetch->backendSquash.mispredictInst->ftqIdx,
            //     ftq->get_curr_pred_idx());

            free_s1_ftq_entries = ftq->get_squash_s1_free_entries(fromFetch->
                                    backendSquash.mispredictInst->ftqIdx);
            if (free_s1_ftq_entries.size() > 0){
                for (auto& free_entry : free_s1_ftq_entries){
                    free_s1_tage_sc_i_bInfos.push_back(
                                    free_entry.tage_sc_i_bInfo);
                }
            }

            free_s2_ftq_entries = ftq->get_squash_s2_free_entries(fromFetch->
                                    backendSquash.mispredictInst->ftqIdx);
            if (free_s2_ftq_entries.size() > 0){
                for (auto& free_entry : free_s2_ftq_entries){
                    free_s2_tage_sc_i_bInfos.push_back(
                                    free_entry.tage_sc_i_bInfo);
                }
            }

            free_s3_ftq_entries = ftq->get_squash_s3_free_entries(fromFetch->
                                    backendSquash.mispredictInst->ftqIdx);
            if (free_s3_ftq_entries.size() > 0){
                for (auto& free_entry : free_s3_ftq_entries){
                    free_s3_tage_sc_i_bInfos.push_back(
                                    free_entry.tage_sc_i_bInfo);
                }
            }

        }else if (fromFetch->backendSquash.squashInst) {
            trace_confidence = true;
            squash_info.is_squash = true;
            squash_info.isMispred = false;
            squash_info.isCond =
                    fromFetch->backendSquash.squashInst->isCondCtrl();
            squash_info.isCall =
                    fromFetch->backendSquash.squashInst->isCall();
            squash_info.isRet =
                    fromFetch->backendSquash.squashInst->isReturn() &&
                    fromFetch->backendSquash.squashInst->isIndirectCtrl();
            if (squash_info.isCall){
                if (fromFetch->backendSquash.squashInst->compressed()){
                    squash_info.callPushAddr =
                                    fromFetch->backendSquash.squashInst->
                                    pcState().instAddr() + 2;
                }else{
                    squash_info.callPushAddr =
                                    fromFetch->backendSquash.squashInst->
                                    pcState().instAddr() + 4;
                }
            }else{
                squash_info.callPushAddr = 0;
            }
            ftq_entry =
                ftq->get_ftq_entry(fromFetch->
                backendSquash.squashInst->ftqIdx);
            squash_info.startAddr = ftq_entry.startAddr;
            squash_info.ftqIdx = ftq_entry.ftq_idx;
            instr_offset = (fromFetch->backendSquash.squashInst->
                        pcState().instAddr() - ftq_entry.startAddr) >> 1;
            squash_info.offset = instr_offset;
            cfi_info.valid = false;
            cfi_info.cfiIndex = 0;
            if (fromFetch->backendSquash.squashItSelf) {
                if (ftq_entry.cfiIndex.valid &&
                    (instr_offset <= ftq_entry.cfiIndex.cfiIndex)) {
                    ftq->write_cfiindex_to_ftq(fromFetch->
                        backendSquash.squashInst->ftqIdx, cfi_info);
                }
            } else {
                if (ftq_entry.cfiIndex.valid &&
                    (instr_offset < ftq_entry.cfiIndex.cfiIndex)) {
                    ftq->write_cfiindex_to_ftq(fromFetch->
                        backendSquash.squashInst->ftqIdx, cfi_info);
                }
            }

            squash_info.tage_sc_i_bInfo =
                                    ftq_entry.tage_sc_i_bInfo;
            squash_info.numBrBefore = 0;
            for (unsigned i = 0; i < MaxNumBr; i++){
                squash_info.condTarget[i] = 0;
                if (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->btb_hit &&
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->hitBtbEntry.slot[i].valid &&
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->hitBtbEntry.slot[i].offset <
                        instr_offset){
                    squash_info.numBrBefore++;
                    squash_info.condTarget[i] = ftq_entry.tage_sc_i_bInfo.
                                        tagebtbBranchInfo->btbBranchInfo->
                                        hitBtbEntry.slot[i].target;
                    if (i == MaxNumBr-1){
                        assert(ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                            btbBranchInfo->hitBtbEntry.slot[i].sharing);
                    }
                }
            }
            //free mem
            // DPRINTF(DecoupledBPU,
            //     "squash ftqIdx: %d, pred_idx: %d\n",
            //     fromFetch->backendSquash.squashInst->ftqIdx,
            //     ftq->get_curr_pred_idx());

            free_s1_ftq_entries = ftq->get_squash_s1_free_entries(fromFetch->
                                    backendSquash.squashInst->ftqIdx);
            if (free_s1_ftq_entries.size() > 0){
                for (auto& free_entry : free_s1_ftq_entries){
                    free_s1_tage_sc_i_bInfos.push_back(
                                free_entry.tage_sc_i_bInfo);
                }
            }

            free_s2_ftq_entries = ftq->get_squash_s2_free_entries(fromFetch->
                                    backendSquash.squashInst->ftqIdx);
            if (free_s2_ftq_entries.size() > 0){
                for (auto& free_entry : free_s2_ftq_entries){
                    free_s2_tage_sc_i_bInfos.push_back(
                                free_entry.tage_sc_i_bInfo);
                }
            }

            free_s3_ftq_entries = ftq->get_squash_s3_free_entries(fromFetch->
                                    backendSquash.squashInst->ftqIdx);
            if (free_s3_ftq_entries.size() > 0){
                for (auto& free_entry : free_s3_ftq_entries){
                    free_s3_tage_sc_i_bInfos.push_back(
                                free_entry.tage_sc_i_bInfo);
                }
            }

        }else{ //start pc squash
            squash_info.is_squash = false;
        }

    }else if (fromFetch->preDecSquash.squash){  //predecode squash
        DPRINTF(DecoupledBPU, "PreDec squash at pc: %#x\n",
                    fromFetch->preDecSquash.branchAddr);
        DPRINTF(DecoupledBPU, "PreDec squash target: %#x\n",
                    fromFetch->preDecSquash.target);
        DPRINTF(DecoupledBPU, "PreDec squash at ftqIdx: %d\n",
                    fromFetch->preDecSquash.ftqIdx);
        DPRINTF(DecoupledBPU, "PreDec squash at seqNum: %d\n",
                    fromFetch->preDecSquash.seqNum);
        trace_confidence = true;
        squash_info.is_squash = fromFetch->preDecSquash.squash;  //tid=0
        squash_info.actual_taken = fromFetch->preDecSquash.branchTaken;
        squash_info.tid = 0;
        //in case when the cond br target is wrong
        squash_info.isCond = fromFetch->preDecSquash.squashInst->isCondCtrl();
        squash_info.isCall = fromFetch->preDecSquash.squashInst->isCall();
        squash_info.isRet = fromFetch->preDecSquash.squashInst->isReturn() &&
                        fromFetch->preDecSquash.squashInst->isIndirectCtrl();
        if (squash_info.isCall){
            if (fromFetch->preDecSquash.squashInst->compressed()){
                squash_info.callPushAddr =
                                fromFetch->preDecSquash.branchAddr + 2;
            }else{
                squash_info.callPushAddr =
                                fromFetch->preDecSquash.branchAddr + 4;
            }
        }else{
            squash_info.callPushAddr = 0;
        }
        ftq_entry = ftq->get_ftq_entry(fromFetch->preDecSquash.ftqIdx);
        squash_info.tage_sc_i_bInfo = ftq_entry.tage_sc_i_bInfo;
        squash_info.isMispred = true;
        instr_offset = (fromFetch->preDecSquash.branchAddr-
                            ftq_entry.startAddr) >> 1;
        squash_info.startAddr = ftq_entry.startAddr;
        squash_info.ftqIdx = ftq_entry.ftq_idx;
        squash_info.offset = instr_offset;
        squash_info.numBrBefore = 0;
        auto btb_info = ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                btbBranchInfo;
        DPRINTF(DecoupledBPU, "btb hit=%d\n", btb_info->btb_hit);

        for (unsigned i = 0; i < MaxNumBr; i++){
            DPRINTF(DecoupledBPU,
                        "hitBtbEntry, slot[%d]_valid=%d, \
                        slot_offset=%d, slot_target=%x, slot_sharing=%d\n",
                        i,
                        btb_info->hitBtbEntry.slot[i].valid,
                        btb_info->hitBtbEntry.slot[i].offset,
                        btb_info->hitBtbEntry.slot[i].target,
                        btb_info->hitBtbEntry.slot[i].sharing);
            squash_info.condTarget[i] = 0;
            if (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->btb_hit &&
                ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[i].valid &&
                ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[i].offset < instr_offset){
                squash_info.numBrBefore++;
                squash_info.condTarget[i] = ftq_entry.tage_sc_i_bInfo.
                                        tagebtbBranchInfo->btbBranchInfo->
                                        hitBtbEntry.slot[i].target;
                DPRINTF(DecoupledBPU,
                        "hitBtbEntry, slot[%d]_valid=%d, \
                        slot_offset=%d, slot_target=%x, slot_sharing=%d\n",
                        i,
                        btb_info->hitBtbEntry.slot[i].valid,
                        btb_info->hitBtbEntry.slot[i].offset,
                        btb_info->hitBtbEntry.slot[i].target,
                        btb_info->hitBtbEntry.slot[i].sharing);
                if (i == MaxNumBr-1){
                    assert(ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                            btbBranchInfo->hitBtbEntry.slot[i].sharing);
                }
            }
        }

        /** update cfiindex in ftq */
        cfi_info.cfiIndex =
            (fromFetch->preDecSquash.branchAddr
                                - ftq_entry.startAddr) >> 1;

        if ((!ftq_entry.cfiIndex.valid ||
            cfi_info.cfiIndex < ftq_entry.cfiIndex.cfiIndex) &&
            fromFetch->preDecSquash.branchTaken){
                cfi_info.valid = true;
                ftq->write_cfiindex_to_ftq(fromFetch->preDecSquash.ftqIdx,
                                            cfi_info);
                DPRINTF(DecoupledBPU,
                "write cfiindex preDecSquash ftqIdx: %d, branchAddr: %x; "
                "startAddr: %x, cfi_info.valid:%d, cfi_info.cfiIndex:%d\n",
                      fromFetch->preDecSquash.ftqIdx,
                      fromFetch->preDecSquash.branchAddr,
                      ftq_entry.startAddr, cfi_info.valid, cfi_info.cfiIndex);
        }else if (ftq_entry.cfiIndex.valid &&
                    cfi_info.cfiIndex <= ftq_entry.cfiIndex.cfiIndex &&
                    !fromFetch->preDecSquash.branchTaken){
                cfi_info.valid = false;
                ftq->write_cfiindex_to_ftq(fromFetch->preDecSquash.ftqIdx,
                                            cfi_info);
                DPRINTF(DecoupledBPU,
                "write cfiindex preDecSquash ftqIdx: %d, branchAddr: %x; "
                "startAddr: %x, cfi_info.valid:%d, cfi_info.cfiIndex:%d\n",
                      fromFetch->preDecSquash.ftqIdx,
                      fromFetch->preDecSquash.branchAddr,
                      ftq_entry.startAddr, cfi_info.valid, cfi_info.cfiIndex);
        }

        /** update target in ftq */
        ftq->write_target_to_ftq(fromFetch->preDecSquash.ftqIdx,
                                    fromFetch->preDecSquash.target);
        //free mem
        // DPRINTF(DecoupledBPU,
        //         "squash ftqIdx: %d, pred_idx: %d\n",
        //         fromFetch->preDecSquash.ftqIdx,
        //         ftq->get_curr_pred_idx());

        free_s1_ftq_entries = ftq->get_squash_s1_free_entries(fromFetch->
                                        preDecSquash.ftqIdx);
        if (free_s1_ftq_entries.size() > 0){
            for (auto& free_entry : free_s1_ftq_entries){
                free_s1_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }

        free_s2_ftq_entries = ftq->get_squash_s2_free_entries(fromFetch->
                                        preDecSquash.ftqIdx);
        if (free_s2_ftq_entries.size() > 0){
            for (auto& free_entry : free_s2_ftq_entries){
                free_s2_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }

        free_s3_ftq_entries = ftq->get_squash_s3_free_entries(fromFetch->
                                        preDecSquash.ftqIdx);
        if (free_s3_ftq_entries.size() > 0){
            for (auto& free_entry : free_s3_ftq_entries){
                free_s3_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }

    }else{
        squash_info.is_squash = false;
    }

    /******** write predecode info to ftq ********/
    if (fromFetch->pdInfo.valid){
        DPRINTF(DecoupledBPU, "pdinfo ftqIdx is: %d\n",
                     fromFetch->pdInfo.ftqIdx);
        DPRINTF(DecoupledBPU,
            "hasJmp: %d; jmpAttr: %d; jmpOffset: %d; \
            isDirect: %d; false hit: %d\n",
            fromFetch->pdInfo.hasJmp, fromFetch->pdInfo.jmpAttr,
            fromFetch->pdInfo.jmpOffset, fromFetch->pdInfo.isDirect,
            fromFetch->pdInfo.false_hit);
        assert(!fromFetch->pdInfo.hasJmp ||
                    (fromFetch->pdInfo.jmpAttr<5));
        /** update PdInfo in ftq */
        ftq->write_pdinfo_to_ftq(fromFetch->pdInfo.ftqIdx,
                                    fromFetch->pdInfo);

        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            DPRINTF(DecoupledBPU, "pdinfo valid is: %d\n",
                        fromFetch->pdInfo.validInstr[i]);
        }

        /** update hitstatus */
        if (fromFetch->pdInfo.false_hit){
            ftq->write_hitstatus_to_ftq(fromFetch->pdInfo.ftqIdx,
                                        FTQ::FtqHitStatus::H_FALSE_HIT);
        }

        /** update commitstatus */
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            if (fromFetch->pdInfo.validInstr[i]){
                ftq->write_commitstatus_to_ftq(fromFetch->pdInfo.ftqIdx,
                                            FTQ::FtqCommitState::VALID, i);
            }else{
                ftq->write_commitstatus_to_ftq(fromFetch->pdInfo.ftqIdx,
                                            FTQ::FtqCommitState::INVALID, i);
            }
        }
    }

    /******** commit ********/
    FTQ::FtqEntry commit_idx_entry;
    commit_idx_entry = ftq->get_commit_idx_entry();
    DPRINTF(DecoupledBPU, "ftq to free entry startAddr is: %x, \
            ftq to free entry commit_idx is: %d\n",
            commit_idx_entry.startAddr, commit_idx_entry.ftq_idx);
    // for (unsigned i = 0; i <= 15; ++i){
    //      DPRINTF(DecoupledBPU, "ftq commitstatus is: %d\n",
    //                 commit_idx_entry.commit_status[i]);
    // }
    for (auto& commitInst : fromFetch->cmtInfo){
        if (commitInst){
            /** update commitstatus */
            ftq_entry = ftq->get_ftq_entry(commitInst->ftqIdx);
            instr_offset = (commitInst->pcState().instAddr() -
                                ftq_entry.startAddr) >> 1;
            DPRINTF(DecoupledBPU, "instr commit pc is: %x, ftqIdx is: %d, \
                    startAddr is: %x, dir is: %d, offset: %d\n",
                    commitInst->pcState().instAddr(), commitInst->ftqIdx,
                    ftq_entry.startAddr, ftq_entry.cfiIndex.valid,
                    ftq_entry.cfiIndex.cfiIndex);
            assert(instr_offset < 16);
            auto staticInst = commitInst->staticInst;
            if (!staticInst->isMicroop() || staticInst->isLastMicroop()) {
                assert(ftq_entry.commit_status[instr_offset] ==
                                        FTQ::FtqCommitState::VALID);
                ftq->write_commitstatus_to_ftq(commitInst->ftqIdx,
                    FTQ::FtqCommitState::COMMITED, instr_offset);
                if (commitInst->isCondCtrl()){
                    ++stats.condCommitted;
                    if (ftq_entry.mispred[instr_offset]){
                        ++stats.condIncorrect;
                        if (!ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                            btbBranchInfo->btb_hit){
                                ++stats.condIncorrect_for_btb_miss;
                        }
                        if (ftq_entry.target >
                                    commitInst->pcState().instAddr()){
                            if ((ftq_entry.target -
                                commitInst->pcState().instAddr() < 7*6*2) &&
                                ftq_entry.cfiIndex.valid){
                                ++stats.condIncorrect_for_sfb;
                            }
                        }
                    }
                }
                if (commitInst->isIndirectCtrl()){
                    ++stats.indirCommitted;
                    if (ftq_entry.mispred[instr_offset]){
                        ++stats.indirIncorrect;
                    }
                    if (commitInst->isReturn()){
                        ++stats.retCommitted;
                        if (ftq_entry.mispred[instr_offset]){
                            ++stats.retIncorrect;
                        }
                    }
                }
            }
        }
    }

    /** gen update info*/
    update_info.valid = false;
    ftq_entry = ftq->get_commit_idx_entry();
    if (ftq->next_entry_has_committed() &&
        ftq->curr_entry_has_committed() &&
        !interrupt_pending){
        update_info.valid = ftq->curr_entry_has_committed() &&
                            ftq->curr_entry_should_up_bpu();
        if (ftq_entry.hasMispred()){
            if (ftq_entry.confidence){
                ++stats.confHighMisp;
            }else{
                ++stats.confLowMisp;
            }
        }else{
            if (ftq_entry.confidence){
                ++stats.confHighRight;
            }else{
                ++stats.confLowRight;
            }
        }
    }

    if (update_info.valid){
        BTBEntry new_btb_entry;
        if (ftq_entry.tage_sc_i_bInfo.ubtbBranchInfo->ubtb_hit){
            ++stats.ubtbCommitHit;
        }else{
            ++stats.ubtbCommitMiss;
        }
        DPRINTF(DecoupledBPU,
            "update_info ftqIdx:%d, \
            hit status =%d, \
            ftq_entry.cfiIndex.valid=%d, \
            ftq_entry.cfiIndex.cfiIndex=%d\n",
            ftq->get_curr_commit_idx(),
            ftq_entry.hit_status,
            ftq_entry.cfiIndex.valid,
            ftq_entry.cfiIndex.cfiIndex);
        BTBEntry realtime_old_btb_entry;
        bool btb_realtime_hit;
        bool slot0_false_empty = false;
        btb_realtime_hit =
                    ftq_entry.hit_status == FTQ::FtqHitStatus::H_HIT;
        realtime_old_btb_entry =
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry;
        unsigned alloc_update_btb = new_btb_gen(ftq_entry.startAddr,
                realtime_old_btb_entry,
                ftq_entry.pd_info,
                ftq_entry.cfiIndex,
                ftq_entry.target,
                btb_realtime_hit,
                ftq_entry.mispred,
                new_btb_entry,
                slot0_false_empty);
        update_info.startAddr = ftq_entry.startAddr;
        update_info.ftqIdx = ftq_entry.ftq_idx;
        update_info.slot0_false_empty = slot0_false_empty;
        update_info.tid = 0;
        update_info.tage_sc_i_bInfo = ftq_entry.tage_sc_i_bInfo;
        update_info.new_btb_entry = new_btb_entry;
        if (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->btb_hit){
            stats.BTBHits++;
        }
        stats.updateNum++;
        DPRINTF(DecoupledBPU,
            "bpu pred_taken:%d,\
            bpu pred taken_offset=%d\n",
            ftq_entry.pred_taken,
            ftq_entry.taken_offset);
        DPRINTF(DecoupledBPU,
            "old_btb_entry hit:%d,\
            hit_status=%d,\
            slot0.valid=%d,\
            slot0.offset=%d,\
            slot1.valid=%d,\
            slot1.offset=%d\n",
            ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
            btbBranchInfo->btb_hit,
            ftq_entry.hit_status,
            ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                btbBranchInfo->hitBtbEntry.slot[0].valid,
            ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                btbBranchInfo->hitBtbEntry.slot[0].offset,
            ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                btbBranchInfo->hitBtbEntry.slot[1].valid,
            ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                btbBranchInfo->hitBtbEntry.slot[1].offset
            );
        DPRINTF(DecoupledBPU,
            "alloc_update_btb:%d, \
            slot0.valid=%d, \
            slot0.offset=%d, \
            slot1.valid=%d,\
            slot1.offset=%d\n",
            alloc_update_btb,
            new_btb_entry.slot[0].valid,
            new_btb_entry.slot[0].offset,
            new_btb_entry.slot[1].valid,
            new_btb_entry.slot[1].offset
            );
        if (alloc_update_btb == 1){
            update_info.allocate_btb = true;
            update_info.update_btb = false;
            update_info.false_hit =
                ftq_entry.hit_status == FTQ::FtqHitStatus::H_FALSE_HIT;
        }else if (alloc_update_btb == 2){
            update_info.allocate_btb = false;
            update_info.update_btb = true;
            update_info.false_hit = false;
        }else{
            update_info.allocate_btb = false;
            update_info.update_btb = false;
            update_info.false_hit = false;
        }

        if (update_info.allocate_btb || update_info.update_btb){
            for (unsigned i = 0; i <= MaxNumBr-1; ++i){
                update_info.actual_taken[i] = new_btb_entry.slot[i].valid &&
                                        ftq_entry.cfiIndex.valid &&
                                        (new_btb_entry.slot[i].offset ==
                                        ftq_entry.cfiIndex.cfiIndex);
                update_info.isCond[i] = new_btb_entry.slot[i].valid &&
                                        (new_btb_entry.slot[i].sharing ||
                                        i != (MaxNumBr-1)) &&
                                        ftq_entry.commit_status[new_btb_entry.
                                        slot[i].offset]
                                        == FTQ::FtqCommitState::COMMITED;
                update_info.isAlwaysTaken[i] =
                                        new_btb_entry.slot[i].always_taken;
            }
            update_info.isIndir = new_btb_entry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (new_btb_entry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !new_btb_entry.slot[1].sharing &&
                                  new_btb_entry.isJalr &&
                                  !new_btb_entry.isRet;
            update_info.isCall = new_btb_entry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (new_btb_entry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !new_btb_entry.slot[1].sharing &&
                                  new_btb_entry.isCall;
            update_info.isRet = new_btb_entry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (new_btb_entry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !new_btb_entry.slot[1].sharing &&
                                  new_btb_entry.isRet;
            update_info.indir_target = new_btb_entry.slot[1].target;
        }else{
            for (unsigned i = 0; i <= MaxNumBr-1; ++i){
                update_info.actual_taken[i] =
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->hitBtbEntry.slot[i].valid &&
                    ftq_entry.cfiIndex.valid &&
                    (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[i].offset ==
                    ftq_entry.cfiIndex.cfiIndex);

                update_info.isCond[i] =
                    ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[i].valid &&
                    (ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[i].sharing ||
                    i != (MaxNumBr-1)) &&
                    ftq_entry.commit_status[ftq_entry.tage_sc_i_bInfo.
                    tagebtbBranchInfo->btbBranchInfo->
                    hitBtbEntry.slot[i].offset]
                    == FTQ::FtqCommitState::COMMITED;
                update_info.isAlwaysTaken[i] = ftq_entry.tage_sc_i_bInfo.
                                            tagebtbBranchInfo->btbBranchInfo->
                                            hitBtbEntry.slot[i].always_taken;
                DPRINTF(DecoupledBPU,
                        "old_btb_entry.slot[%d].always_taken:%d\n",
                        i, ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                        btbBranchInfo->hitBtbEntry.slot[i].always_taken);
            }
            update_info.isIndir = ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (ftq_entry.tage_sc_i_bInfo.
                                   tagebtbBranchInfo->
                                   btbBranchInfo->hitBtbEntry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].sharing &&
                                  ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.isJalr &&
                                  !ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.isRet;
            update_info.isCall = ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (ftq_entry.tage_sc_i_bInfo.
                                   tagebtbBranchInfo->
                                   btbBranchInfo->hitBtbEntry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].sharing &&
                                  ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.isCall;
            update_info.isRet = ftq_entry.tage_sc_i_bInfo.tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].valid &&
                                  ftq_entry.cfiIndex.valid &&
                                  (ftq_entry.tage_sc_i_bInfo.
                                   tagebtbBranchInfo->
                                   btbBranchInfo->hitBtbEntry.slot[1].offset ==
                                  ftq_entry.cfiIndex.cfiIndex) &&
                                  !ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.slot[1].sharing &&
                                  ftq_entry.tage_sc_i_bInfo.
                                  tagebtbBranchInfo->
                                  btbBranchInfo->hitBtbEntry.isRet;
            update_info.indir_target = ftq_entry.tage_sc_i_bInfo.
                                       tagebtbBranchInfo->
                                       btbBranchInfo->
                                       hitBtbEntry.slot[1].target;
        }
    }

    /** free ftq entry when all instrs in this block committed*/
    /** free should be one ftq entry delay*/
    if (ftq->next_entry_has_committed() &&
        ftq->curr_entry_has_committed() &&
        !interrupt_pending){
        //free committed entry
        ftq_entry = ftq->get_commit_idx_entry();
        free_s1_tage_sc_i_bInfos.push_back(ftq_entry.tage_sc_i_bInfo);
        free_s2_tage_sc_i_bInfos.push_back(ftq_entry.tage_sc_i_bInfo);
        free_s3_tage_sc_i_bInfos.push_back(ftq_entry.tage_sc_i_bInfo);
        //update commit_ptr
        ftq->commit_retire_entry();
    }

    if (fromFetch->interruptPending){
        interrupt_pending = true;
    }

    if (fromFetch->clearInterrupt){
        interrupt_pending = false;
    }

    if (interrupt_pending){
        DPRINTF(DecoupledBPU, "interrupt pending, stop pred\n");
    }
    if (ftq->is_ftq_full()){
        DPRINTF(DecoupledBPU, "ftq is full, stop pred\n");
    }

    ThreadID tid = 0;
    std::vector<unsigned> intra_squashed_ftq_idxs;
    FTQ::FtqEntry ftq_entry_pred_previous;

    /** squash recover/commit update/lookup for s3 predict */
    bool pred_enable_s3;
    bool slot1_is_indir_s3;
    bool slot1_is_call_s3;
    bool slot1_is_ret_s3;
    bool confidence_s3[MaxNumBr];
    bool pred_isCond_s3[MaxNumBr];
    bool pred_cond_taken_s3[MaxNumBr];
    bool press_from_s3 = false;

    pred_enable_s3 = fromS2->pred_enable &&
                    !fromFetch->backendSquash.squash &&
                    !fromFetch->preDecSquash.squash;
    ftq_entry_pred_s3 = fromS2->ftq_entry;
    slot1_is_indir_s3 = fromS2->slot1_is_indir;
    slot1_is_call_s3 = fromS2->slot1_is_call;
    slot1_is_ret_s3 = fromS2->slot1_is_ret;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        confidence_s3[i] = fromS2->confidence[i];
        pred_isCond_s3[i] = fromS2->pred_isCond[i];
        pred_cond_taken_s3[i] = fromS2->pred_cond_taken[i];
    }

    tage_sc_i_blk->tick_s3(squash_info, update_info,
                            ftq_entry_pred_s3.tage_sc_i_bInfo,
                            ftq_entry_pred_s3.startAddr,
                            ftq_entry_pred_s3.endAddr,
                            ftq_entry_pred_s3.target,
                            ftq_entry_pred_s3.cfiIndex.valid,
                            ftq_entry_pred_s3.cfiIndex.cfiIndex,
                            tid,
                            free_s3_tage_sc_i_bInfos,
                            pred_enable_s3,
                            slot1_is_indir_s3,
                            slot1_is_ret_s3,
                            &pred_isCond_s3[0],
                            &pred_cond_taken_s3[0],
                            press_from_s3,
                            ftq_entry_pred_s3.ftq_idx,
                            &confidence_s3[0]);

    bool s3_squash = false;
   /*write s2 info to ftq*/
    if (pred_enable_s3){
        if (!confidence_s3[0] || !confidence_s3[1]){
            trace_confidence = false;
        }
        ftq_entry_pred_s3.confidence = confidence_s3[0] &&
                                    confidence_s3[1] && trace_confidence;
        ftq_entry_pred_s3.pred_taken = ftq_entry_pred_s3.cfiIndex.valid;
        ftq_entry_pred_s3.taken_offset = ftq_entry_pred_s3.cfiIndex.cfiIndex;
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            ftq_entry_pred_s3.mispred[i] = false;
            ftq_entry_pred_s3.commit_status[i] =
                                    FTQ::FtqCommitState::UNFETCHED;
        }
        ftq_entry_pred_s3.fetch_status = FTQ::FtqFetchStatus::F_TO_SEND;

        if (ftq_entry_pred_s3.tage_sc_i_bInfo.tagebtbBranchInfo->
                                            btbBranchInfo->btb_hit){
            if (ftq_entry_pred_s3.startAddr >= ftq_entry_pred_s3.endAddr){
                ftq_entry_pred_s3.hit_status = FTQ::FtqHitStatus::H_FALSE_HIT;
            }else{
                ftq_entry_pred_s3.hit_status = FTQ::FtqHitStatus::H_HIT;
            }
        }else{
            ftq_entry_pred_s3.hit_status = FTQ::FtqHitStatus::H_NOT_HIT;
        }
        FTQ::FtqEntry ftq_entry_pred_previous =
                            ftq->get_ftq_entry(ftq_entry_pred_s3.ftq_idx);
        //s3_squash gen
        if (ftq_entry_pred_s3.pred_taken){
            if (!ftq_entry_pred_previous.pred_taken ||
                ftq_entry_pred_s3.cfiIndex.cfiIndex !=
                            ftq_entry_pred_previous.cfiIndex.cfiIndex ||
                ftq_entry_pred_s3.target !=
                                    ftq_entry_pred_previous.target){
                s3_squash = true;
            }
        }else{
            if (ftq_entry_pred_previous.pred_taken ||
                ftq_entry_pred_s3.endAddr !=
                                    ftq_entry_pred_previous.endAddr){
                s3_squash = true;
            }
        }

        if (s3_squash){
            squash_info.tid = 0;
            squash_info.tage_sc_i_bInfo = ftq_entry_pred_s3.tage_sc_i_bInfo;
            squash_info.actual_taken = ftq_entry_pred_s3.pred_taken;
            if (!fromS2->pred_cond_taken[0] && pred_cond_taken_s3[0] &&
                pred_isCond_s3[0]){
                squash_info.is_squash = true;
                squash_info.isCond = true;
                squash_info.isCall = false;
                squash_info.isRet = false;
                squash_info.isMispred = true;
                squash_info.numBrBefore = 0;
            }else if (fromS2->pred_cond_taken[0] && !pred_cond_taken_s3[0] &&
                    pred_isCond_s3[0]){
                squash_info.is_squash = true;
                squash_info.isCond = pred_isCond_s3[1];
                squash_info.isCall = slot1_is_call_s3;
                squash_info.isRet = slot1_is_ret_s3;
                squash_info.isMispred = true;
                squash_info.numBrBefore = 1;
            }else if (fromS2->pred_cond_taken[0] == pred_cond_taken_s3[0] &&
                     pred_isCond_s3[0] && pred_isCond_s3[1] &&
                     fromS2->pred_cond_taken[1] != pred_cond_taken_s3[1]){
                squash_info.is_squash = true;
                squash_info.isCond = true;
                squash_info.isCall = false;
                squash_info.isRet = false;
                squash_info.isMispred = true;
                squash_info.numBrBefore = 1;
            }else{
                DPRINTF(DecoupledBPU,
                    "pred_isCond_s3[1]=%d, slot1_is_indir_s3=%d, \
                    slot1_is_ret_s3=%d\n",
                    pred_isCond_s3[1], slot1_is_indir_s3,
                    slot1_is_ret_s3);
                // assert(!pred_isCond_s3[1] && slot1_is_indir_s3 &&
                //         !slot1_is_ret_s3);
                squash_info.is_squash = false;
                squash_info.isCond = false;
                squash_info.isCall = slot1_is_call_s3;
                squash_info.isRet = false;
                squash_info.isMispred = true;
                squash_info.numBrBefore = pred_isCond_s3[0] ? 1 : 0;
            }
            squash_info.callPushAddr = ftq_entry_pred_s3.tage_sc_i_bInfo.
                                            tagebtbBranchInfo->btbBranchInfo->
                                            hitBtbEntry.fallThruAddr;
            squash_info.startAddr = ftq_entry_pred_s3.startAddr;
            squash_info.ftqIdx = ftq_entry_pred_s3.ftq_idx;
            squash_info.offset = 0; //not used
            for (unsigned i = 0; i < MaxNumBr; ++i){
                squash_info.condTarget[i] = ftq_entry_pred_s3.tage_sc_i_bInfo.
                                            tagebtbBranchInfo->btbBranchInfo->
                                            hitBtbEntry.slot[i].target;
            }
            ++stats.s3_redir;
            DPRINTF(DecoupledBPU, "s3 squash happens\n");
        }

        ftq->s3_update_to_ftq(ftq_entry_pred_s3);

        DPRINTF(DecoupledBPU,
                    "s3 pred ftq pushed, ftqIdx=%d, startAddr=%x, \
                    taken=%d, taken pos=%d, endAddr=%x, target=%x\n",
                    ftq_entry_pred_s3.ftq_idx, ftq_entry_pred_s3.startAddr,
                    ftq_entry_pred_s3.pred_taken,
                    ftq_entry_pred_s3.cfiIndex.cfiIndex,
                    ftq_entry_pred_s3.endAddr,
                    ftq_entry_pred_s3.target);

        assert(ftq_entry_pred_s3.startAddr ==
                                ftq_entry_pred_previous.startAddr);
        assert(ftq_entry_pred_s3.endAddr ==
                                ftq_entry_pred_previous.endAddr);
    }

    //free mem
    if (s3_squash){
        free_s1_ftq_entries = ftq->
                        get_squash_s1_free_entries(ftq_entry_pred_s3.ftq_idx);
        if (free_s1_ftq_entries.size() > 0){
            for (auto& free_entry : free_s1_ftq_entries){
                free_s1_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }

        free_s2_ftq_entries = ftq->
                        get_squash_s2_free_entries(ftq_entry_pred_s3.ftq_idx);
        if (free_s2_ftq_entries.size() > 0){
            for (auto& free_entry : free_s2_ftq_entries){
                free_s2_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }
    }

    /** squash recover/commit update/lookup for s2 predict */
    bool pred_enable_s2;
    bool slot1_is_indir_s2;
    bool slot1_is_call_s2;
    bool slot1_is_ret_s2;
    bool confidence_s2[MaxNumBr];
    bool pred_isCond_s2[MaxNumBr];
    bool pred_cond_taken_s2[MaxNumBr];

    pred_enable_s2 = fromS1->pred_enable &&
                    !fromFetch->backendSquash.squash &&
                    !fromFetch->preDecSquash.squash && !s3_squash;
    ftq_entry_pred_s2 = fromS1->ftq_entry;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        confidence_s2[i] = fromS1->confidence[i];
    }


    tage_sc_i_blk->tick_s2(squash_info, update_info,
                            ftq_entry_pred_s2.tage_sc_i_bInfo,
                            ftq_entry_pred_s2.startAddr,
                            ftq_entry_pred_s2.endAddr,
                            ftq_entry_pred_s2.target,
                            ftq_entry_pred_s2.cfiIndex.valid,
                            ftq_entry_pred_s2.cfiIndex.cfiIndex,
                            tid,
                            free_s2_tage_sc_i_bInfos,
                            pred_enable_s2,
                            slot1_is_indir_s2,
                            slot1_is_call_s2,
                            slot1_is_ret_s2,
                            &pred_isCond_s2[0],
                            &pred_cond_taken_s2[0],
                            &confidence_s2[0]);

    bool s2_squash = false;
   /*write s2 info to ftq*/
    if (pred_enable_s2){
        //s2 need info
        ftq_entry_pred_s2.confidence = confidence_s2[0] &&
                                    confidence_s2[1] && trace_confidence;
        ftq_entry_pred_s2.pred_taken = ftq_entry_pred_s2.cfiIndex.valid;
        ftq_entry_pred_s2.taken_offset = ftq_entry_pred_s2.cfiIndex.cfiIndex;
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            ftq_entry_pred_s2.mispred[i] = false;
            ftq_entry_pred_s2.commit_status[i] =
                                    FTQ::FtqCommitState::UNFETCHED;
        }
        ftq_entry_pred_s2.fetch_status = FTQ::FtqFetchStatus::F_TO_SEND;

        if (ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                                            btbBranchInfo->btb_hit){
            if (ftq_entry_pred_s2.startAddr >= ftq_entry_pred_s2.endAddr){
                ftq_entry_pred_s2.hit_status = FTQ::FtqHitStatus::H_FALSE_HIT;
            }else{
                ftq_entry_pred_s2.hit_status = FTQ::FtqHitStatus::H_HIT;
            }
        }else{
            ftq_entry_pred_s2.hit_status = FTQ::FtqHitStatus::H_NOT_HIT;
        }

        ftq_entry_pred_previous =
                            ftq->get_ftq_entry(ftq_entry_pred_s2.ftq_idx);
        //s2_squash gen
        if (!s3_squash){
            if (ftq_entry_pred_s2.pred_taken){
                if (!ftq_entry_pred_previous.pred_taken ||
                    ftq_entry_pred_s2.cfiIndex.cfiIndex !=
                                ftq_entry_pred_previous.cfiIndex.cfiIndex ||
                    ftq_entry_pred_s2.target !=
                                        ftq_entry_pred_previous.target){
                    s2_squash = true;
                }
            }else{
                if (ftq_entry_pred_previous.pred_taken ||
                    ftq_entry_pred_s2.endAddr !=
                                        ftq_entry_pred_previous.endAddr){
                    s2_squash = true;
                }
            }
        }
        ++stats.ubtbLookups;
        if (s2_squash){
            ++stats.s2_redir;
            DPRINTF(DecoupledBPU, "s2 squash happens\n");
        }

        ftq->s2_update_to_ftq(ftq_entry_pred_s2);

        DPRINTF(DecoupledBPU,
                    "s2 pred ftq pushed, ftqIdx=%d, startAddr=%x, \
                    taken=%d, taken pos=%d, endAddr=%x, target=%x\n",
                    ftq_entry_pred_s2.ftq_idx, ftq_entry_pred_s2.startAddr,
                    ftq_entry_pred_s2.pred_taken,
                    ftq_entry_pred_s2.cfiIndex.cfiIndex,
                    ftq_entry_pred_s2.endAddr,
                    ftq_entry_pred_s2.target);

        DPRINTF(DecoupledBPU,
                    "s2 pred ftq pushed, btb hit=%d, slot0_valid=%d, \
                    slot0_offset=%d, slot0_target=%x\n",
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->btb_hit,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[0].valid,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[0].offset,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[0].target);

        DPRINTF(DecoupledBPU,
                    "s2 pred ftq pushed, btb hit=%d, slot1_valid=%d, \
                    slot1_offset=%d, slot1_target=%x, slot1_sharing=%d\n",
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->btb_hit,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[1].valid,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[1].offset,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[1].target,
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.slot[1].sharing);

        DPRINTF(DecoupledBPU,
                    "s2 pred ftq pushed, fallthrough addr=%x\n",
                    ftq_entry_pred_s2.tage_sc_i_bInfo.tagebtbBranchInfo->
                    btbBranchInfo->hitBtbEntry.fallThruAddr);
        assert(ftq_entry_pred_s2.startAddr ==
                                ftq_entry_pred_previous.startAddr);
    }

    //free mem
    if (s2_squash){
        free_s1_ftq_entries = ftq->
                        get_squash_s1_free_entries(ftq_entry_pred_s2.ftq_idx);
        if (free_s1_ftq_entries.size() > 0){
            for (auto& free_entry : free_s1_ftq_entries){
                free_s1_tage_sc_i_bInfos.push_back(free_entry.tage_sc_i_bInfo);
            }
        }
    }

    toS3->pred_enable = pred_enable_s2;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        toS3->confidence[i] = confidence_s2[i];
    }
    toS3->ftq_entry = ftq_entry_pred_s2;
    toS3->slot1_is_indir = slot1_is_indir_s2;
    toS3->slot1_is_call = slot1_is_call_s2;
    toS3->slot1_is_ret = slot1_is_ret_s2;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        toS3->pred_isCond[i] = pred_isCond_s2[i];
        toS3->pred_cond_taken[i] = pred_cond_taken_s2[i];
    }

    /** squash recover/commit update/lookup for s1 predict */
    bool pred_enable_s1 = !ftq->is_ftq_full() &&
                       !fromFetch->backendSquash.squash &&
                       !fromFetch->preDecSquash.squash &&
                       !s2_squash && !s3_squash &&
                       !interrupt_pending &&
                       !(update_info.valid &&
                       (update_info.allocate_btb || update_info.update_btb)) &&
                       !press_from_s3;
    bool confidence_s1[MaxNumBr];

    tage_sc_i_blk->tick_s1(squash_info, update_info,
                        ftq_entry_pred_s1.tage_sc_i_bInfo,
                        ftq_entry_pred_s1.startAddr,
                        ftq_entry_pred_s1.endAddr,
                        ftq_entry_pred_s1.target,
                        ftq_entry_pred_s1.cfiIndex.valid,
                        ftq_entry_pred_s1.cfiIndex.cfiIndex,
                        tid,
                        free_s1_tage_sc_i_bInfos,
                        pred_enable_s1,
                        &confidence_s1[0]);

    /** write s1 pred results to ftq */
    if (pred_enable_s1){
        //s1 need info to fetch
        ftq_entry_pred_s1.confidence = confidence_s1[0] &&
                                    confidence_s1[1] && trace_confidence;
        ftq_entry_pred_s1.pred_taken = ftq_entry_pred_s1.cfiIndex.valid;
        ftq_entry_pred_s1.taken_offset = ftq_entry_pred_s1.cfiIndex.cfiIndex;
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            ftq_entry_pred_s1.mispred[i] = false;
            ftq_entry_pred_s1.commit_status[i] =
                                        FTQ::FtqCommitState::UNFETCHED;
        }
        ftq_entry_pred_s1.fetch_status = FTQ::FtqFetchStatus::F_TO_SEND;

        ftq->push_back_to_ftq(ftq_entry_pred_s1);
        DPRINTF(DecoupledBPU,
                    "s1 pred ftq pushed, ftqIdx=%d, startAddr=%x, \
                    taken=%d, taken pos=%d, endAddr=%x\n",
                    ftq_entry_pred_s1.ftq_idx, ftq_entry_pred_s1.startAddr,
                    ftq_entry_pred_s1.pred_taken,
                    ftq_entry_pred_s1.cfiIndex.cfiIndex,
                    ftq_entry_pred_s1.endAddr);
        DPRINTF(DecoupledBPU,
                    "s1 pred ftq pushed, btb hit=%d, slot0_valid=%d, \
                    slot0_offset=%d, slot0_target=%x\n",
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->ubtb_hit,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[0].valid,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[0].offset,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[0].target);

        DPRINTF(DecoupledBPU,
                    "s1 pred ftq pushed, btb hit=%d, slot1_valid=%d, \
                    slot1_offset=%d, slot1_target=%x, slot1_sharing=%d\n",
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->ubtb_hit,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[1].valid,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[1].offset,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[1].target,
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.slot[1].sharing);

        DPRINTF(DecoupledBPU,
                    "s1 pred ftq pushed, fallthrough addr=%x\n",
                    ftq_entry_pred_s1.tage_sc_i_bInfo.ubtbBranchInfo->
                    ubtb_hit_entry.fallThruAddr);
    }

    toS2->pred_enable = pred_enable_s1;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        toS2->confidence[i] = confidence_s1[i];
    }
    toS2->ftq_entry = ftq_entry_pred_s1;

    if (fromFetch->backendSquash.squash){
        /** recover ftq ptr */
        if (fromFetch->backendSquash.squashInst){
            ftq->recover_fetch_ptr(ftq->next_idx(fromFetch->
                            backendSquash.squashInst->ftqIdx));
            ftq->recover_prefetch_ptr(ftq->next_idx(fromFetch->
                            backendSquash.squashInst->ftqIdx));
            ftq->recover_pred_s1_ptr(ftq->next_idx(fromFetch->
                            backendSquash.squashInst->ftqIdx));
            ftq->recover_pred_s2_ptr(ftq->next_idx(fromFetch->
                            backendSquash.squashInst->ftqIdx));
            ftq->recover_pred_s3_ptr(ftq->next_idx(fromFetch->
                            backendSquash.squashInst->ftqIdx));
        }else{
            ftq->recover_fetch_ptr(0);
            ftq->recover_prefetch_ptr(0);
            ftq->recover_pred_s1_ptr(0);
            ftq->recover_pred_s2_ptr(0);
            ftq->recover_pred_s3_ptr(0);
        }

        /** gen next startAddr for pred*/
        ftq_entry_pred_s1.startAddr = fromFetch->backendSquash.pc->instAddr();
    } else if (fromFetch->preDecSquash.squash){
         /** recover ftq ptr */
         ftq->recover_fetch_ptr(
                    ftq->next_idx(fromFetch->preDecSquash.ftqIdx));
         ftq->recover_prefetch_ptr(
                    ftq->next_idx(fromFetch->preDecSquash.ftqIdx));
         ftq->recover_pred_s1_ptr(
                    ftq->next_idx(fromFetch->preDecSquash.ftqIdx));
         ftq->recover_pred_s2_ptr(
                    ftq->next_idx(fromFetch->preDecSquash.ftqIdx));
         ftq->recover_pred_s3_ptr(
                    ftq->next_idx(fromFetch->preDecSquash.ftqIdx));
         /** gen next startAddr for pred*/
         ftq_entry_pred_s1.startAddr = fromFetch->preDecSquash.target;
    }else{
        /** gen next startAddr for pred*/
        if (s3_squash){
            if (ftq->is_before_fetch_idx(ftq_entry_pred_s3.ftq_idx)){
                intra_squashed_ftq_idxs = ftq->get_intra_squashed_ftq_idxs(
                                                ftq_entry_pred_s3.ftq_idx);
            }
            if (ftq->is_before_fetch_idx(
                        ftq_entry_pred_s3.ftq_idx)){
                ftq->recover_fetch_ptr(
                        ftq_entry_pred_s3.ftq_idx);
            }
            if (ftq->is_before_prefetch_idx(
                        ftq_entry_pred_s3.ftq_idx)){
                ftq->recover_prefetch_ptr(
                        ftq_entry_pred_s3.ftq_idx);
            }
            ftq->recover_pred_s1_ptr(ftq->next_idx(ftq_entry_pred_s3.ftq_idx));
            ftq->recover_pred_s2_ptr(ftq->next_idx(ftq_entry_pred_s3.ftq_idx));
            if (ftq_entry_pred_s3.pred_taken){
                ftq_entry_pred_s1.startAddr = ftq_entry_pred_s3.target;
            }else{
                ftq_entry_pred_s1.startAddr = ftq_entry_pred_s3.endAddr;
            }
        }else if (s2_squash){
            if (ftq->is_before_fetch_idx(ftq_entry_pred_s2.ftq_idx)){
                intra_squashed_ftq_idxs = ftq->get_intra_squashed_ftq_idxs(
                                                ftq_entry_pred_s2.ftq_idx);
            }
            if (ftq->is_before_fetch_idx(
                        ftq_entry_pred_s2.ftq_idx)){
                ftq->recover_fetch_ptr(
                        ftq_entry_pred_s2.ftq_idx);
            }
            if (ftq->is_before_prefetch_idx(
                        ftq_entry_pred_s2.ftq_idx)){
                ftq->recover_prefetch_ptr(
                        ftq_entry_pred_s2.ftq_idx);
            }
            ftq->recover_pred_s1_ptr(ftq->next_idx(ftq_entry_pred_s2.ftq_idx));
            if (ftq_entry_pred_s2.pred_taken){
                ftq_entry_pred_s1.startAddr = ftq_entry_pred_s2.target;
            }else{
                ftq_entry_pred_s1.startAddr = ftq_entry_pred_s2.endAddr;
            }
        }else{
            if (pred_enable_s1){
                if (ftq_entry_pred_s1.pred_taken){
                    ftq_entry_pred_s1.startAddr = ftq_entry_pred_s1.target;
                }else{
                    ftq_entry_pred_s1.startAddr = ftq_entry_pred_s1.endAddr;
                }
            }
        }
    }

    /** gen fetch info to ifetch */
    if (!ftq->is_ftq_fetch_empty() && !fromFetch->bpuBlock &&
        !fromFetch->backendSquash.squash && !fromFetch->preDecSquash.squash){
        ftq_entry_fetch = ftq->fetch_pop_from_ftq();
        toFetch->instBlkFetchInfo.valid = true;
        toFetch->instBlkFetchInfo.confidence = ftq_entry_fetch.confidence;
        toFetch->instBlkFetchInfo.ftqIdx = ftq_entry_fetch.ftq_idx;
        toFetch->instBlkFetchInfo.startAddr = ftq_entry_fetch.startAddr;
        toFetch->instBlkFetchInfo.endAddr = ftq_entry_fetch.endAddr;
        toFetch->instBlkFetchInfo.target = ftq_entry_fetch.target;
        toFetch->instBlkFetchInfo.predTaken = ftq_entry_fetch.pred_taken;
        toFetch->instBlkFetchInfo.cfiIndex =
                                    ftq_entry_fetch.cfiIndex.cfiIndex;
    }

    /** gen prefetch info to ifetch */
    if (!ftq->is_ftq_prefetch_empty() && !fromFetch->bpuPrefetchBlock &&
        !fromFetch->backendSquash.squash && !fromFetch->preDecSquash.squash){
        ftq_entry_prefetch = ftq->prefetch_pop_from_ftq();
        toFetch->instBlkPrefetchInfo.valid = true;
        if (ftq_entry_prefetch.pred_taken){
            toFetch->instBlkPrefetchInfo.startAddr = ftq_entry_prefetch.target;
        }else{
            toFetch->instBlkPrefetchInfo.startAddr =
                                                    ftq_entry_prefetch.endAddr;
        }
    }

    /** gen intra squash to ifetch */
    toFetch->intra_squash = s2_squash || s3_squash;
    toFetch->intra_squashed_ftq_idxs = intra_squashed_ftq_idxs;
    if (s3_squash){
        DPRINTF(DecoupledBPU, "s3 intra_squashed_ftq_idxs size is: %d\n",
                    intra_squashed_ftq_idxs.size());
        if (intra_squashed_ftq_idxs.size() > 0){
            for (auto& entry_idx : intra_squashed_ftq_idxs){
                DPRINTF(DecoupledBPU, "s3 intra_squashed_ftq_idx is: %d\n",
                    entry_idx);
            }
        }
        assert(intra_squashed_ftq_idxs.size() <= 2);
    }
    if (s2_squash){
        DPRINTF(DecoupledBPU, "s2 intra_squashed_ftq_idxs size is: %d\n",
                    intra_squashed_ftq_idxs.size());
        if (intra_squashed_ftq_idxs.size() > 0){
            for (auto& entry_idx : intra_squashed_ftq_idxs){
                DPRINTF(DecoupledBPU, "s2 intra_squashed_ftq_idx is: %d\n",
                    entry_idx);
            }
        }
        assert(intra_squashed_ftq_idxs.size() <= 1);
    }

    S2Queue.advance();
    S1Queue.advance();

    DPRINTF(DecoupledBPU, "current pred_s1_idx is: %d\n",
                    ftq->get_curr_pred_s1_idx());
    DPRINTF(DecoupledBPU, "current pred_s2_idx is: %d\n",
                    ftq->get_curr_pred_s2_idx());
    DPRINTF(DecoupledBPU, "current pred_s3_idx is: %d\n",
                    ftq->get_curr_pred_s3_idx());
    DPRINTF(DecoupledBPU, "current fetch_idx is: %d\n",
                    ftq->get_curr_fetch_idx());
    DPRINTF(DecoupledBPU, "current prefetch_idx is: %d\n",
                    ftq->get_curr_prefetch_idx());
    DPRINTF(DecoupledBPU, "current commit_idx is: %d\n",
                    ftq->get_curr_commit_idx());
}

void DecoupledBPU::advance(){

}


} // namespace branch_prediction
} // namespace gem5

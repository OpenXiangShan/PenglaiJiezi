/*
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

#include "cpu/pred/dppred_blk/ubtb_blk.hh"

#include <cstdlib>

#include "base/intmath.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/Ubtb.hh"

namespace gem5
{

namespace branch_prediction
{

UBTB_BLK::UBTB_BLK(const UBTB_BLKParams &p)
    : SimObject(p),
      numEntries(p.numEntries),
      ways(p.ways),
      tagBits(p.tagBits),
      instShiftAmt(p.instShiftAmt),
      log2NumThreads(floorLog2(p.num_threads)),
      ctrWidth(p.ctrWidth),
      predBlkSize(p.predBlkSize),
      stats(this)
{

    if (!isPowerOf2(numEntries)) {
        fatal("UBTB entries is not a power of 2!");
    }
    if (!isPowerOf2(ways)) {
        fatal("UBTB ways is not a power of 2!");
    }

    btb = new BTBEntry*[ways];
    ctr = new UBTBCTR*[ways];
    lru = new uint8_t*[ways];
    for (int i = 0; i <= ways-1; i++) {
        btb[i] = new BTBEntry [numEntries/ways];
        ctr[i] = new UBTBCTR [numEntries/ways];
        lru[i] = new uint8_t [numEntries/ways];
    }
    for (unsigned i = 0; i <= ways-1; ++i) {
        for (unsigned j = 0; j <= numEntries/ways-1; ++j) {
            lru[i][j] = 0;
        }
    }

    idxMask = numEntries/ways - 1;

    tagMask = (1 << tagBits) - 1;

    tagShiftAmt = instShiftAmt + floorLog2(numEntries/ways);
}

UBTB_BLK::~UBTB_BLK(){

    for (unsigned i = 0; i <= ways-1; ++i) {
        delete []btb[i];
        delete []ctr[i];
        delete []lru[i];
    }
    delete []btb;
    delete []ctr;
    delete []lru;
}

UBTB_BLK::UBTBBLKStats::UBTBBLKStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(ubtb_false_hit, statistics::units::Count::get(),
               "Number of ubtb false hit"),
      ADD_STAT(ubtb_ctr_update, statistics::units::Count::get(),
               "Number of ubtb ctr update")
{
}

// Up-down saturating counter
template<typename T>
void
UBTB_BLK::ctrUpdate(T & ctr, bool taken, int nbits)
{
    assert(nbits <= sizeof(T) << 3);
    if (taken) {
        if (ctr < ((1 << (nbits - 1)) - 1))
            ctr++;
    } else {
        if (ctr > -(1 << (nbits - 1)))
            ctr--;
    }
}

// int8_t and int versions of this function may be needed
template void UBTB_BLK::ctrUpdate(int8_t & ctr, bool taken, int nbits);
template void UBTB_BLK::ctrUpdate(int & ctr, bool taken, int nbits);

// Up-down unsigned saturating counter
void
UBTB_BLK::unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits)
{
    assert(nbits <= sizeof(uint8_t) << 3);
    if (up) {
        if (ctr < ((1 << nbits) - 1))
            ctr++;
    } else {
        if (ctr)
            ctr--;
    }
}

void
UBTB_BLK::reset()
{
    for (unsigned i = 0; i <= ways-1; ++i) {
        for (unsigned j = 0; j <= numEntries/ways-1; ++j) {
            btb[i][j].valid = false;
        }
    }
}


unsigned
UBTB_BLK::getIndex(Addr instPC, ThreadID tid)
{
    // Need to shift PC over by the word offset.
    return ((instPC >> instShiftAmt)
            ^ (tid << (tagShiftAmt - instShiftAmt - log2NumThreads)))
            & idxMask;
}


Addr
UBTB_BLK::getTag(Addr instPC)
{
    return (instPC >> tagShiftAmt) & tagMask;
}

bool
UBTB_BLK::valid(Addr instPC, ThreadID tid)
{
    unsigned btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    assert(btb_idx < numEntries/ways);
    bool btb_hit = false;

    for (unsigned i = 0; i <= ways-1; ++i) {
        if (btb[i][btb_idx].valid
            && inst_tag == btb[i][btb_idx].tag
            && btb[i][btb_idx].tid == tid) {
            btb_hit = true;
            break;
        }
    }
    return btb_hit;
}

void
UBTB_BLK::lookup(Addr instPC, ThreadID tid,
            BTBEntry *&hit_btb_entry, UBTBCTR *&hit_ctr)
{
    unsigned btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    hit_btb_entry = &btb[0][0];
    hit_ctr = &ctr[0][0];

    assert(btb_idx < numEntries/ways);

    for (unsigned i = 0; i <= ways-1; ++i) {
        if (btb[i][btb_idx].valid
            && inst_tag == btb[i][btb_idx].tag
            && btb[i][btb_idx].tid == tid) {
            hit_btb_entry = &btb[i][btb_idx];
            hit_ctr = &ctr[i][btb_idx];
            unsignedCtrUpdate(lru[i][btb_idx], true, lruBits);
            bool lru_sat = true;
            for (unsigned j = 0; j <= ways-1; ++j) {
                if (lru[j][btb_idx] == 0){
                    lru_sat = false;
                    break;
                }
            }
            if (lru_sat){
                for (unsigned j = 0; j <= ways-1; ++j) {
                    unsignedCtrUpdate(lru[j][btb_idx], false, lruBits);
                }
            }
            break;
        }
    }
}


bool
UBTB_BLK::predict(Addr &startAddr, Addr &endAddr,
                 Addr &target, unsigned &pred_offset,
                 ThreadID tid, UBTBBranchInfo* &bi,
                 bool &taken_is_indir, bool &taken_is_ret,
                 bool &taken_is_call, bool *isCond,
                 bool *pred_taken, bool *confidence)
{

    bi = new UBTBBranchInfo;

    BTBEntry *btb_hit_entry;
    UBTBCTR *ctr_hit;
    lookup(startAddr, tid, btb_hit_entry, ctr_hit);
    bi->ubtb_hit_entry = *btb_hit_entry;

    bool isAlwaysTaken[MaxNumBr];
    bool dir_pred[MaxNumBr];
    bool predTaken;

    if (!valid(startAddr, tid)){
        isCond[0] = false;
        isCond[1] = false;
        isAlwaysTaken[0] = false;
        isAlwaysTaken[1] = false;
        bi->ubtb_hit = false;
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
        bi->ubtb_hit = true;
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
        DPRINTF(Ubtb, "ubtb slot0 valid is %d\n",
                        btb_hit_entry->slot[0].valid);
        DPRINTF(Ubtb, "ubtb slot0 offset is %d\n",
                        btb_hit_entry->slot[0].offset);
        DPRINTF(Ubtb, "ubtb slot0 always taken is %d\n",
                        btb_hit_entry->slot[0].always_taken);
        DPRINTF(Ubtb, "ubtb slot0 sharing is %d\n",
                        btb_hit_entry->slot[0].sharing);
        DPRINTF(Ubtb, "ubtb slot1 valid is %d\n",
                        btb_hit_entry->slot[1].valid);
        DPRINTF(Ubtb, "ubtb slot1 offset is %d\n",
                        btb_hit_entry->slot[1].offset);
        DPRINTF(Ubtb, "ubtb slot1 always taken is %d\n",
                        btb_hit_entry->slot[1].always_taken);
        DPRINTF(Ubtb, "ubtb slot1 sharing is %d\n",
                        btb_hit_entry->slot[1].sharing);
        DPRINTF(Ubtb, "ubtb slot1 is call %d\n",
                        btb_hit_entry->isCall);
        DPRINTF(Ubtb, "ubtb slot0 cond is %d, slot1 cond is %d\n",
                        isCond[0], isCond[1]);
    }


    for (unsigned i = 0; i < MaxNumBr; ++i){
        dir_pred[i] = ctr_hit->ctr[i] >= 0;
        pred_taken[i] = dir_pred[i] || isAlwaysTaken[i];
    }

    if (!valid(startAddr, tid)){
        predTaken = false;
        pred_offset = 0;
        target = startAddr + predBlkSize;
        endAddr = startAddr + predBlkSize;
    } else {
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
            if (!btb_hit_entry->slot[1].sharing && btb_hit_entry->isJalr){
                taken_is_indir = true;
                if (btb_hit_entry->isRet){
                    taken_is_ret = true;
                }
            }
            if (!btb_hit_entry->slot[1].sharing && btb_hit_entry->isCall){
                taken_is_call = true;
            }
        } else {
            predTaken = false;
            pred_offset = 0;
            target = btb_hit_entry->fallThruAddr;
        }
        endAddr = btb_hit_entry->fallThruAddr;
    }
    return predTaken;
}



bool
UBTB_BLK::alloc_update_lookup(Addr instPC,
            ThreadID tid, BTBEntry &hit_btb_entry)
{
    unsigned btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    for (unsigned i = 0; i <= ways-1; ++i) {
        if (btb[i][btb_idx].valid
            && inst_tag == btb[i][btb_idx].tag
            && btb[i][btb_idx].tid == tid) {
            hit_btb_entry = btb[i][btb_idx];
            return true;
        }
    }
    return false;
}

BTBEntry
UBTB_BLK::allocate_merge(Addr start_addr,
            BTBEntry old_entry, BTBEntry new_entry){
    BTBEntry merged_entry = old_entry;
    if (old_entry.slot[0].valid &&
        new_entry.slot[0].valid &&
        new_entry.slot[0].offset < old_entry.slot[0].offset){
        //insert to slot0
        merged_entry.slot[0] = new_entry.slot[0];
        merged_entry.slot[1] = old_entry.slot[0];
        merged_entry.slot[1].sharing = true;
        merged_entry.isCall = false;
        merged_entry.isRet = false;
        merged_entry.isJalr = false;
        if (old_entry.slot[1].valid){
            merged_entry.fallThruAddr = start_addr +
                                        old_entry.slot[1].offset*2;
        }else{
            merged_entry.fallThruAddr = start_addr +
                                        MaxInstrsPerBlock*2;
        }
    }else if (!old_entry.slot[0].valid &&
                new_entry.slot[0].valid &&
                old_entry.slot[1].valid &&
                new_entry.slot[0].offset < old_entry.slot[1].offset){
        //write to slot0
        merged_entry.slot[0] = new_entry.slot[0];
    }else if (new_entry.slot[0].valid &&
                old_entry.slot[0].valid &&
                new_entry.slot[0].offset > old_entry.slot[0].offset &&
                old_entry.slot[1].valid &&
                new_entry.slot[0].offset < old_entry.slot[1].offset){
        //insert to slot1
        merged_entry.slot[1] = new_entry.slot[0];
        merged_entry.slot[1].sharing = true;
        merged_entry.isCall = false;
        merged_entry.isRet = false;
        merged_entry.isJalr = false;
        merged_entry.fallThruAddr = start_addr +
                                    old_entry.slot[1].offset*2;
    }else if (new_entry.slot[0].valid &&
                old_entry.slot[0].valid &&
                new_entry.slot[0].offset > old_entry.slot[0].offset &&
                !old_entry.slot[1].valid){
        //write to slot1
        merged_entry.slot[1] = new_entry.slot[0];
        merged_entry.slot[1].sharing = true;
        merged_entry.isCall = false;
        merged_entry.isRet = false;
        merged_entry.isJalr = false;
        merged_entry.fallThruAddr = start_addr +
                                        MaxInstrsPerBlock*2;
    }else if (new_entry.slot[0].valid &&
                old_entry.slot[1].valid &&
                old_entry.slot[0].valid &&
                new_entry.slot[0].offset > old_entry.slot[1].offset){
        merged_entry.fallThruAddr = start_addr +
                                    new_entry.slot[0].offset*2;
    }
    return merged_entry;
}


void
UBTB_BLK::allocate_btb_entry(Addr inst_pc, BTBEntry new_btb_entry,
                        ThreadID tid, bool false_hit)
{
    unsigned btb_idx = getIndex(inst_pc, tid);
    Addr inst_tag = getTag(inst_pc);
    BTBEntry old_btb_entry;

    assert(btb_idx < numEntries/ways);
    bool allocated = false;

    if (valid(inst_pc, tid)){
        //update read
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (btb[i][btb_idx].valid
                && inst_tag == btb[i][btb_idx].tag
                && btb[i][btb_idx].tid == tid) {
                if (!false_hit){
                    old_btb_entry = btb[i][btb_idx];
                    //epecial process for milc
                    btb[i][btb_idx] = allocate_merge(inst_pc,
                                    old_btb_entry, new_btb_entry);
                    for (unsigned br = 0; br < MaxNumBr; ++br) {
                        ctr[i][btb_idx].ctr[br] = 0;
                    }
                }else{
                    btb[i][btb_idx] = new_btb_entry;
                    for (unsigned br = 0; br < MaxNumBr; ++br) {
                        ctr[i][btb_idx].ctr[br] = 0;
                    }
                }
                allocated = true;
                break;
            }
        }
    }else{
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (!btb[i][btb_idx].valid){
                btb[i][btb_idx] = new_btb_entry;
                for (unsigned br = 0; br < MaxNumBr; ++br) {
                    ctr[i][btb_idx].ctr[br] = 0;
                }
                lru[i][btb_idx] = ((1 << lruBits) - 1);
                allocated = true;
                DPRINTF(Ubtb, "allocate empty ubtb, startAddr=%x\n", inst_pc);
                break;
            }
        }
    }

    if (!allocated){
        //choose a replace way
        unsigned replace_way = 0;
        uint8_t lru_value = ((1 << lruBits) - 1);
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (lru[i][btb_idx] < lru_value){
                lru_value = lru[i][btb_idx];
                replace_way = i;
            }
        }
        //replace_way = rand() & (ways-1);
        btb[replace_way][btb_idx] = new_btb_entry;
        for (unsigned br = 0; br < MaxNumBr; ++br) {
            ctr[replace_way][btb_idx].ctr[br] = 0;
        }
        lru[replace_way][btb_idx] = ((1 << lruBits) - 1);
        DPRINTF(Ubtb, "allocate non-empty ubtb, startAddr=%x\n", inst_pc);
    }
}

void
UBTB_BLK::update_btb_entry(Addr inst_pc, BTBEntry new_btb_entry, ThreadID tid,
                    bool slot0_false_empty)
{
    unsigned btb_idx = getIndex(inst_pc, tid);
    Addr inst_tag = getTag(inst_pc);
    BTBEntry old_btb_entry;

    assert(btb_idx < numEntries/ways);

    if (valid(inst_pc, tid)){
        //update read
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (btb[i][btb_idx].valid
                && inst_tag == btb[i][btb_idx].tag
                && btb[i][btb_idx].tid == tid) {
                //epecial process for milc
                if (slot0_false_empty && btb[i][btb_idx].slot[0].valid){
                    old_btb_entry = btb[i][btb_idx];
                    btb[i][btb_idx] = allocate_merge(inst_pc,
                                    old_btb_entry, new_btb_entry);
                    for (unsigned br = 0; br < MaxNumBr; ++br) {
                        ctr[i][btb_idx].ctr[br] = 0;
                    }
                }else{
                    btb[i][btb_idx] = new_btb_entry;
                    for (unsigned br = 0; br < MaxNumBr; ++br) {
                        ctr[i][btb_idx].ctr[br] = 0;
                    }
                }
                break;
            }
        }
    }
}


void
UBTB_BLK::update_ctr(Addr inst_pc, ThreadID tid,
                     bool* taken, bool* isCond)
{
    unsigned btb_idx = getIndex(inst_pc, tid);
    Addr inst_tag = getTag(inst_pc);

    assert(btb_idx < numEntries/ways);

    if (valid(inst_pc, tid)){
        //update read
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (btb[i][btb_idx].valid
                && inst_tag == btb[i][btb_idx].tag
                && btb[i][btb_idx].tid == tid) {
                if (isCond[0]){
                    ctrUpdate(ctr[i][btb_idx].ctr[0], taken[0], ctrWidth);
                }
                if ((!isCond[0] || !taken[0]) && isCond[1]){
                    ctrUpdate(ctr[i][btb_idx].ctr[1], taken[1], ctrWidth);
                }
                break;
            }
        }
    }
}

bool
UBTB_BLK::btb_entry_cmp(BTBEntry btb_entry0, BTBEntry btb_entry1){
    bool slot_equal = true;
    for (unsigned i = 0; i < MaxNumBr; ++i){
        if (btb_entry0.slot[i].valid && btb_entry1.slot[i].valid){
            if (btb_entry0.slot[i].target != btb_entry1.slot[i].target ||
                btb_entry0.slot[i].offset != btb_entry1.slot[i].offset ||
                btb_entry0.slot[i].sharing != btb_entry1.slot[i].sharing ||
                btb_entry0.slot[i].always_taken !=
                                        btb_entry1.slot[i].always_taken){
                slot_equal = false;
                break;
            }
        }else if (btb_entry0.slot[i].valid != btb_entry1.slot[i].valid){
            slot_equal = false;
            break;
        }
    }

    return (slot_equal &&
            btb_entry0.tag == btb_entry1.tag &&
            btb_entry0.tid == btb_entry1.tid &&
            btb_entry0.isCall == btb_entry1.isCall &&
            btb_entry0.isRet == btb_entry1.isRet &&
            btb_entry0.isJalr == btb_entry1.isJalr &&
            btb_entry0.fallThruAddr == btb_entry1.fallThruAddr &&
            btb_entry0.valid == btb_entry1.valid);
}


void
UBTB_BLK::update(Addr startAddr, ThreadID tid,
                    bool *actual_taken, bool* isCond,
                    BTBEntry new_btb_entry, UBTBBranchInfo * &bi,
                    bool allocate_btb, bool update_btb, bool false_hit,
                    bool slot0_false_empty)
{
    /** update btb */
    BTBEntry old_btb_entry;
    new_btb_entry.tag = getTag(startAddr); //recaculate tag
    if (allocate_btb || !bi->ubtb_hit){
        //ubtb miss & btb hit/ubtb miss & btb miss
        allocate_btb_entry(startAddr, new_btb_entry, tid, false_hit);
        DPRINTF(Ubtb, "allocate ubtb, startAddr=%x\n", startAddr);
        if (!allocate_btb && !update_btb){
            DPRINTF(Ubtb, "ubtb miss, btb hit\n");
        }
    }else if (update_btb){
        update_btb_entry(startAddr, new_btb_entry, tid, slot0_false_empty);
        DPRINTF(Ubtb, "update ubtb, startAddr=%x\n", startAddr);
    }else{
        if (alloc_update_lookup(startAddr, tid, old_btb_entry)){
            if (btb_entry_cmp(old_btb_entry, new_btb_entry)){
                update_ctr(startAddr, tid, actual_taken, isCond);
                DPRINTF(Ubtb, "update ubtb ctr, startAddr=%x\n", startAddr);
                ++stats.ubtb_ctr_update;
            }else{
                allocate_btb_entry(startAddr, new_btb_entry, tid, true);
                DPRINTF(Ubtb, "ubtb false hit, startAddr=%x\n", startAddr);
                ++stats.ubtb_false_hit;
            }
        }
    }
}

void
UBTB_BLK::free_mem(UBTBBranchInfo * &bi){
    /** free memory*/
    delete bi;
}

} // namespace branch_prediction
} // namespace gem5

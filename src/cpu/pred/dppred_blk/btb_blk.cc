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

#include "cpu/pred/dppred_blk/btb_blk.hh"

#include <cstdlib>

#include "base/intmath.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/Fetch.hh"

namespace gem5
{

namespace branch_prediction
{

BTB_BLK::BTB_BLK(unsigned _numBr,
                unsigned _numEntries,
                unsigned _ways,
                unsigned _tagBits,
                unsigned _instShiftAmt,
                unsigned _num_threads)
    : numBr(_numBr),
      numEntries(_numEntries),
      ways(_ways),
      tagBits(_tagBits),
      instShiftAmt(_instShiftAmt),
      log2NumThreads(floorLog2(_num_threads))
{
    DPRINTF(Fetch, "BTB: Creating BTB object.\n");

    if (!isPowerOf2(numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }
    if (!isPowerOf2(ways)) {
        fatal("BTB ways is not a power of 2!");
    }

    btb = new BTBEntry*[ways];
    lru = new uint8_t*[ways];
    for (int i = 0; i <= ways-1; i++) {
        btb[i] = new BTBEntry [numEntries/ways];
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

BTB_BLK::~BTB_BLK(){
    for (unsigned i = 0; i <= ways-1; ++i) {
        delete []btb[i];
        delete []lru[i];
    }
    delete []btb;
    delete []lru;
}

// Up-down unsigned saturating counter
void
BTB_BLK::unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits)
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
BTB_BLK::reset()
{
    for (unsigned i = 0; i <= ways-1; ++i) {
        for (unsigned j = 0; j <= numEntries/ways-1; ++j) {
            btb[i][j].valid = false;
        }
    }
}


unsigned
BTB_BLK::getIndex(Addr instPC, ThreadID tid)
{
    // Need to shift PC over by the word offset.
    return ((instPC >> instShiftAmt)
            ^ (tid << (tagShiftAmt - instShiftAmt - log2NumThreads)))
            & idxMask;
}


Addr
BTB_BLK::getTag(Addr instPC)
{
    return (instPC >> tagShiftAmt) & tagMask;
}

bool
BTB_BLK::valid(Addr instPC, ThreadID tid)
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

BTBEntry
*BTB_BLK::lookup(Addr instPC, ThreadID tid)
{
    unsigned btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    BTBEntry *btb_hit_entry = &btb[0][0];

    assert(btb_idx < numEntries/ways);

    for (unsigned i = 0; i <= ways-1; ++i) {
        if (btb[i][btb_idx].valid
            && inst_tag == btb[i][btb_idx].tag
            && btb[i][btb_idx].tid == tid) {
            btb_hit_entry = &btb[i][btb_idx];
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
    return btb_hit_entry;
}

bool
BTB_BLK::alloc_update_lookup(Addr instPC,
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
BTB_BLK::allocate_merge(Addr start_addr,
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
BTB_BLK::allocate(Addr inst_pc, BTBEntry new_btb_entry,
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
                }else{
                    btb[i][btb_idx] = new_btb_entry;
                }
                allocated = true;
                break;
            }
        }
    }else{
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (!btb[i][btb_idx].valid){
                btb[i][btb_idx] = new_btb_entry;
                lru[i][btb_idx] = ((1 << lruBits) - 1);
                allocated = true;
                break;
            }
        }
    }

    //int replace_way = random_mt.random<int>() & (ways-1);
    //int replace_way = rand() & (ways-1);
    if (!allocated){
        //choose a replace way
        unsigned replace_way = 0;
        uint8_t lru_value = ((1 << lruBits) - 1);
        for (unsigned i = 0; i <= ways-1; ++i) {
            if (lru[i][btb_idx] < lru_value){
                lru_value = lru[i][btb_idx] ;
                replace_way = i;
            }
        }
        //replace_way = rand() & (ways-1);
        btb[replace_way][btb_idx] = new_btb_entry;
        lru[replace_way][btb_idx] = ((1 << lruBits) - 1);
    }
}

void
BTB_BLK::update(Addr inst_pc, BTBEntry new_btb_entry, ThreadID tid,
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
                }else{
                    btb[i][btb_idx] = new_btb_entry;
                }
                break;
            }
        }
    }

}


} // namespace branch_prediction
} // namespace gem5

/*
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/store_set.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/StoreSet.hh"

namespace gem5
{

namespace o3
{

StoreSet::StoreSet(uint64_t clear_period, int64_t clear_period_thres,
                int _SSIT_size, int _LFST_size, int _LFST_entry_size)
    : clearPeriod(clear_period), clearPeriodThreshold(clear_period_thres),
    SSITSize(_SSIT_size), LFSTSize(_LFST_size), LFSTEntrySize(_LFST_entry_size)
{
    DPRINTF(StoreSet, "StoreSet: Creating store set object.\n");
    DPRINTF(StoreSet, "StoreSet: SSIT size: %i, LFST size: %i.\n",
            SSITSize, LFSTSize);

    if (!isPowerOf2(SSITSize)) {
        fatal("Invalid SSIT size!\n");
    }

    SSIT.resize(SSITSize);

    validSSIT.resize(SSITSize);

    SSITStrict.resize(SSITSize);

    for (int i = 0; i < SSITSize; ++i) {
        validSSIT[i] = false;
        SSITStrict[i] = false;
    }

    if (!isPowerOf2(LFSTSize)) {
        fatal("Invalid LFST size!\n");
    }

    LFSTLarge.resize(LFSTSize);

    LFSTLargePC.resize(LFSTSize);

    validLFSTLarge.resize(LFSTSize);

    VictimEntryID.resize(LFSTSize);

    //storeSeqNum.resize(LFSTSize);

    for (int i = 0; i < LFSTSize; ++i) {
        LFSTLarge[i].resize(LFSTEntrySize);
        LFSTLargePC[i].resize(LFSTEntrySize);
        validLFSTLarge[i].resize(LFSTEntrySize);
        VictimEntryID[i] = 0;
        //storeSeqNum[i].resize(LFSTEntrySize);
        for (int j = 0; j < LFSTEntrySize; ++j) {
            validLFSTLarge[i][j] = false;
            LFSTLarge[i][j] = 0;
            LFSTLargePC[i][j] = 0;
            //storeSeqNum[i][j] = 0;
        }
    }

    indexMask = SSITSize - 1;

    offsetBits = 2;

    memOpsPred = 0;

    lastClearPeriodCycle = 0;
}

StoreSet::~StoreSet()
{
}

void
StoreSet::init(uint64_t clear_period, int64_t clear_period_thres,
                 int _SSIT_size, int _LFST_size, int _LFST_entry_size)
{
    SSITSize = _SSIT_size;
    LFSTSize = _LFST_size;
    clearPeriod = clear_period;
    clearPeriodThreshold = clear_period_thres;
    LFSTEntrySize = _LFST_entry_size;

    DPRINTF(StoreSet, "StoreSet: Creating store set object.\n");
    DPRINTF(StoreSet, "StoreSet: SSIT size: %i, LFST size: %i.\n",
            SSITSize, LFSTSize);

    SSIT.resize(SSITSize);

    validSSIT.resize(SSITSize);

    SSITStrict.resize(SSITSize);

    for (int i = 0; i < SSITSize; ++i) {
        validSSIT[i] = false;
        SSITStrict[i] = false;
    }


    LFSTLarge.resize(LFSTSize);

    LFSTLargePC.resize(LFSTSize);

    validLFSTLarge.resize(LFSTSize);

    VictimEntryID.resize(LFSTSize);

    //storeSeqNum.resize(LFSTSize);

    for (int i = 0; i < LFSTSize; ++i) {
        //validLFST[i] = false;
        //LFST[i] = 0;
        LFSTLarge[i].resize(LFSTEntrySize);
        LFSTLargePC[i].resize(LFSTEntrySize);
        validLFSTLarge[i].resize(LFSTEntrySize);
        VictimEntryID[i] = 0;
        //storeSeqNum[i].resize(LFSTEntrySize);
        for (int j = 0; j < LFSTEntrySize; ++j) {
            validLFSTLarge[i][j] = false;
            LFSTLarge[i][j] = 0;
            LFSTLargePC[i][j] = 0;
            //storeSeqNum[i][j] = 0;
        }
    }

    indexMask = SSITSize - 1;

    offsetBits = 2;

    memOpsPred = 0;

    lastClearPeriodCycle = 0;
}


Addr StoreSet::XORFold(uint64_t pc, uint64_t resetWidth) {
   // pc[63:0]: pc[69:60]^pc[59:50]^...
    uint64_t pcWidth = sizeof(pc)*8;
    uint64_t fold_range = (pcWidth + resetWidth - 1) / resetWidth;
    uint64_t xored = 0;
    uint64_t value_low;

    do {
        value_low = pc & ((1 << resetWidth) - 1);
        xored ^= value_low;
        pc >>= resetWidth;
        fold_range --;
    } while (fold_range != 0);

    return xored;
}

void
StoreSet::violation(Addr store_PC, Addr load_PC)
{
    int load_index = calcIndexSSIT(load_PC);
    int store_index = calcIndexSSIT(store_PC);
    DPRINTF(StoreSet, "violation happend, fill SSIT,"
            " load_index:%d, store_index:%d\n",
            load_index,store_index);

    assert(load_index < SSITSize && store_index < SSITSize);

    bool valid_load_SSID = validSSIT[load_index];
    bool valid_store_SSID = validSSIT[store_index];

    if (!valid_load_SSID && !valid_store_SSID) {
        // Calculate a new SSID here.
        SSID ld_new_set = calcSSID(load_PC);
        SSID sd_new_set = calcSSID(store_PC);

        validSSIT[load_index] = true;
        SSIT[load_index] = ld_new_set;

        validSSIT[store_index] = true;
        //SSIT[store_index] = sd_new_set;
        SSIT[store_index] = ld_new_set;

        assert(ld_new_set < LFSTSize);
        assert(sd_new_set < LFSTSize);

        DPRINTF(StoreSet, "StoreSet: Neither load nor store had a valid "
                "storeset, creating new: ld:%i/sd:%i"
                " for load %#x, store %#x\n",
                ld_new_set,sd_new_set, load_PC, store_PC);
    } else if (valid_load_SSID && !valid_store_SSID) {
        SSID load_SSID = SSIT[load_index];

        SSID sd_new_set = calcSSID(store_PC);

        validSSIT[store_index] = true;

        SSIT[store_index] = sd_new_set;

        assert(sd_new_set < LFSTSize);

        DPRINTF(StoreSet, "StoreSet: Load had a valid store set.  Adding "
                "store to that set: %i for load %#x, store %#x\n",
                load_SSID, load_PC, store_PC);
    } else if (!valid_load_SSID && valid_store_SSID) {
        SSID store_SSID = SSIT[store_index];
        SSID ld_new_set = calcSSID(load_PC);

        validSSIT[load_index] = true;

        SSIT[load_index] = ld_new_set;

        DPRINTF(StoreSet, "StoreSet: Store had a valid store set: %i for "
                "load %#x, store %#x\n",
                store_SSID, load_PC, store_PC);
    } else {
        SSID load_SSID = SSIT[load_index];
        SSID store_SSID = SSIT[store_index];

        assert(load_SSID < LFSTSize && store_SSID < LFSTSize);

        // The store set with the lower number wins
        if (store_SSID > load_SSID) {
            SSIT[store_index] = load_SSID;

            DPRINTF(StoreSet, "StoreSet: Load had smaller store set: %i; "
                    "for load %#x, store %#x\n",
                    load_SSID, load_PC, store_PC);
        } else {
            SSIT[load_index] = store_SSID;

            if (store_SSID == load_SSID) {
                SSITStrict[load_index] = true;
            }

            DPRINTF(StoreSet, "StoreSet: Store had smaller store set: %i; "
                    "for load %#x, store %#x\n",
                    store_SSID, load_PC, store_PC);
        }
    }

    DPRINTF(StoreSet, "dump After violation\n");
    dump();
}

void
StoreSet::checkClear(Cycles curCycle)
{
    uint64_t delta_cycle =( uint64_t ) curCycle - lastClearPeriodCycle;

    /*
    memOpsPred++;
    if (memOpsPred > clearPeriod) {
    */
   if (delta_cycle > clearPeriodThreshold) {
        DPRINTF(StoreSet, "curCycle %ld, delta_cycle %ld,"
                "clearPeriodThreshold: %ld\n",
                curCycle, delta_cycle, clearPeriodThreshold);
        DPRINTF(StoreSet, "Wiping predictor state beacuse %d"
                " cycles executed\n",
                clearPeriodThreshold);
        memOpsPred = 0;
        clear();
        lastClearPeriodCycle = ( uint64_t ) curCycle;
    }
}

void
StoreSet::insertLoad(Addr load_PC, InstSeqNum load_seq_num)
{
    Cycles cyc(0);
    checkClear(cyc);
    // Does nothing.
    return;
}

int
StoreSet::findVictimInLFSTEntry(int store_SSID)
{
    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (!validLFSTLarge[store_SSID][j]) {
            return j;
        }
    }

    VictimEntryID[store_SSID]++;
    if (VictimEntryID[store_SSID] >= LFSTEntrySize) {
        VictimEntryID[store_SSID] %= LFSTEntrySize;
    }

    return VictimEntryID[store_SSID];

}

void
StoreSet::insertStore(Addr store_PC, InstSeqNum store_seq_num,
    ThreadID tid, Cycles curCycle)
{
    int index = calcIndexSSIT(store_PC);

    int store_SSID;

    int victim_inst;

    checkClear(curCycle);
    assert(index < SSITSize);

    if (!validSSIT[index]) {
        // Do nothing if there's no valid entry.
        return;
    } else {
        store_SSID = SSIT[index];

        assert(store_SSID < LFSTSize);

        victim_inst = findVictimInLFSTEntry(store_SSID);

        LFSTLarge[store_SSID][victim_inst] = store_seq_num;

        LFSTLargePC[store_SSID][victim_inst] = store_PC;

        validLFSTLarge[store_SSID][victim_inst] = 1;

        DPRINTF(StoreSet, "Store PC %#x updated the LFST, SSID: %i\n",
                store_PC, store_SSID);

        DPRINTF(StoreSet, "dump After insert\n");
        dump();
    }
    DPRINTF(StoreSet, "in func:%s, SSIT[%d]: store_SSID:%d\n",
                __func__, index, store_SSID);
}


bool
StoreSet::checkInstStrict(Addr PC)
{
    int index = calcIndexSSIT(PC);

    bool inst_strict;

    assert(index < SSITSize);

    if (!validSSIT[index]) {
        DPRINTF(StoreSet, "Inst %#x with index %i had no SSID\n",
                PC, index);

        // Return false if there's no valid entry.
        return  false;
    } else {
        inst_strict = SSITStrict[index];

        return inst_strict;
    }
}


std::vector<InstSeqNum>
StoreSet::checkInst(Addr PC)
{
    int index = calcIndexSSIT(PC);

    int inst_SSID;

    assert(index < SSITSize);

    std::vector<InstSeqNum> vec = {};

    DPRINTF(StoreSet, "in func:%s, Checking Inst %#x with index %i\n",
                __func__, PC, index);

    if (!validSSIT[index]) {
        DPRINTF(StoreSet, "Inst %#x with index %i had no SSID\n",
                PC, index);

        // Return 0 if there's no valid entry.
        return  vec;
    } else {
        DPRINTF(StoreSet, "Inst %#x, SSIT[%i] is valid\n",
                PC, index);
        inst_SSID = SSIT[index];

        assert(inst_SSID < LFSTSize);

        for (int j = 0; j < LFSTEntrySize; ++j) {
            if (validLFSTLarge[inst_SSID][j]) {
                DPRINTF(StoreSet, "Inst pc:%#x with SSIT index %i and SSID %i"
                    " had LFST entryID:%d inum of %i pc:%#x\n",
                    PC, index, inst_SSID,
                    j, LFSTLarge[inst_SSID][j], LFSTLargePC[inst_SSID][j]);

                vec.push_back(LFSTLarge[inst_SSID][j]);
            }
            else {
                DPRINTF(StoreSet, "Inst pc:%#x with SSIT index %i and SSID %i"
                    " had LFST entryID:%d inum of %i pc:%#x\n",
                    PC, index, inst_SSID,
                    j, LFSTLarge[inst_SSID][j], LFSTLargePC[inst_SSID][j]);
            }
        }

        return vec;
    }
}

void
StoreSet::issued(Addr issued_PC, InstSeqNum issued_seq_num, bool is_store)
{
    // This only is updated upon a store being issued.
    if (!is_store) {
        return;
    }

    int index = calcIndexSSIT(issued_PC);

    int store_SSID;

    assert(index < SSITSize);

    // Make sure the SSIT still has a valid entry for the issued store.
    if (!validSSIT[index]) {
        return;
    }

    store_SSID = SSIT[index];

    assert(store_SSID < LFSTSize);

    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (validLFSTLarge[store_SSID][j] &&
                LFSTLarge[store_SSID][j] == issued_seq_num) {
            DPRINTF(StoreSet, "StoreSet: store invalidated itself in LFST.\n");
            validLFSTLarge[store_SSID][j] = false;
            LFSTLarge[store_SSID][j] = 0;
            LFSTLargePC[store_SSID][j] = 0;
        }
    }

    DPRINTF(StoreSet, "dump After issued\n");
    dump();
}

void
StoreSet::squash(InstSeqNum squashed_num, ThreadID tid)
{
    DPRINTF(StoreSet, "StoreSet: Squashing until inum %i\n",
            squashed_num);

    for (int i = 0; i < LFSTSize; ++i) {
        for (int j = 0; j < LFSTEntrySize; ++j) {
            if ( validLFSTLarge[i][j] && LFSTLarge[i][j]> squashed_num) {
                DPRINTF(StoreSet, "Squashed [sn:%lli], pc\n",
                    LFSTLarge[i][j],
                    LFSTLargePC[i][j]);

                LFSTLarge[i][j] = 0;
                LFSTLargePC[i][j] = 0;
                validLFSTLarge[i][j] = false;

                //DPRINTF(StoreSet, "After squash, validLFSTLarge\n");
            }
            else if (!validLFSTLarge[i][j]) {
                LFSTLarge[i][j] = 0;
                LFSTLargePC[i][j] = 0;
                //DPRINTF(StoreSet, "After squash, !validLFSTLarge\n");
            }
        }
    }

    DPRINTF(StoreSet, "dump After squash\n");
    dump();
}

void
StoreSet::clear()
{
    for (int i = 0; i < SSITSize; ++i) {
        validSSIT[i] = false;
    }

    for (int i = 0; i < LFSTSize; ++i) {
        for (int j = 0; j < LFSTEntrySize; ++j) {
            validLFSTLarge[i][j] = false;
        }
    }

}

void
StoreSet::dump()
{
    //cprintf("storeList.size(): %i\n", storeList.size());
    int num = 0;
    DPRINTF(StoreSet, "Dumping LFST: \n");
    for (int i = 0; i < LFSTSize; ++i) {
        for (int j = 0; j < LFSTEntrySize; ++j) {
            if (validLFSTLarge[i][j])
                DPRINTF(StoreSet,"%i: SSID:%i, [sn:%lli], pc:%#x\n",
                    num, i, LFSTLarge[i][j], LFSTLargePC[i][j]);

            num++;
        }
    }

    DPRINTF(StoreSet, "Dumping SSIT: \n");
    for (int i = 0; i < SSITSize; ++i) {
        if (validSSIT[i])
            DPRINTF(StoreSet,"entry[%i]:SSID: %d\n",
                i, SSIT[i]);
    }
}

} // namespace o3
} // namespace gem5

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
#include "cpu/pred/dppred_blk/tage_base_blk.hh"
#include <queue>
#include "base/intmath.hh"
#include "base/logging.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "debug/Fetch.hh"
#include "debug/Tage.hh"

namespace gem5
{

namespace branch_prediction
{

TAGEBase_BLK::TAGEBase_BLK(const TAGEBase_BLKParams &p)
   : SimObject(p),
     tagTableCounterBits(p.tagTableCounterBits),
     logRatioBiModalHystEntries(p.logRatioBiModalHystEntries),
     nHistoryTables(p.nHistoryTables),
     baseTableCounterBits(p.baseTableCounterBits),
     tagTableUBits(p.tagTableUBits),
     histBufferSize(p.histBufferSize),
     minHist(p.histLengths[1]),
     maxHist(p.histLengths[p.nHistoryTables]),
     histLengths(p.histLengths),
     pathHistBits(p.pathHistBits),
     numBr(p.numBr),
     tagTableTagWidths(p.tagTableTagWidths),
     logTagTableSizes(p.logTagTableSizes),
     //use construct func to create a vector of p.numThreads entries
     threadHistory(p.numThreads),
     logUResetPeriod(p.logUResetPeriod),
     initialTCounterValue(p.initialTCounterValue),
     numUseAltOnNa(p.numUseAltOnNa),
     useAltOnNaBits(p.useAltOnNaBits),
     maxNumAlloc(p.maxNumAlloc),
     noSkip(p.noSkip),
     speculativeHistUpdate(p.speculativeHistUpdate),
     instShiftAmt(p.instShiftAmt),
     initialized(false),
     stats(this, nHistoryTables)
{
    if (noSkip.empty()) {
        // Set all the table to enabled by default
        noSkip.resize(nHistoryTables + 1, true);
    }
    // Current method for periodically resetting the u counter bits only
    // works for 1 or 2 bits
    // Also make sure that it is not 0
    assert(tagTableUBits <= 2 && (tagTableUBits > 0));

    // we use int type for the path history, so it cannot be more than
    // its size
    assert(pathHistBits <= (sizeof(int)*8));

    // initialize the counter to half of the period
    assert(logUResetPeriod != 0);
    tCounter = initialTCounterValue;
    for (int i = 0; i < MaxNumBr; i++) {
        bankTickCtrs[i] = 0;
    }

    assert(histBufferSize > maxHist * 2);
    //initialize global history of all threads
    for (auto& history : threadHistory) {
        history.pathHist = 0;
        //init global history buffer
        history.globalHistory = new uint8_t[histBufferSize];
        //set pointer of gHist to the head of global history buffer
        history.gHist = history.globalHistory;
        //set values in global history buffer to 0
        memset(history.gHist, 0, histBufferSize);
        //set index of ptGhist to 0
        history.ptGhist = 0;
    }

    assert(tagTableTagWidths.size() == (nHistoryTables+1));
    assert(logTagTableSizes.size() == (nHistoryTables+1));

    // First entry is for the Bimodal table and it is untagged in this
    // implementation
    assert(tagTableTagWidths[0] == 0);

    for (auto& history : threadHistory) {
        history.computeIndices = new FoldedHistory[nHistoryTables+1];
        history.computeTags[0] = new FoldedHistory[nHistoryTables+1];
        history.computeTags[1] = new FoldedHistory[nHistoryTables+1];
        initFoldedHistories(history);  //init all params in folded history
    }
    gtable = new TageEntry *[nHistoryTables + 1];
    buildTageTables();
    tableIndices = new int [nHistoryTables+1];
    tableTags = new int [nHistoryTables+1];
}



void
TAGEBase_BLK::initFoldedHistories(ThreadHistory & history)
{
    for (int i = 1; i <= nHistoryTables; i++) {
        history.computeIndices[i].init(
            histLengths[i], (logTagTableSizes[i]));
        history.computeTags[0][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]);
        history.computeTags[1][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]-1);
        DPRINTF(Tage, "HistLength:%d, TTSize:%d, TTTWidth:%d\n",
                histLengths[i], logTagTableSizes[i], tagTableTagWidths[i]);
    }
}

void
TAGEBase_BLK::buildTageTables()
{
    for (int i = 1; i <= nHistoryTables; i++) {
        //create tage tables
        gtable[i] = new TageEntry[1<<(logTagTableSizes[i])];
        for (int j = 0; j < (1<<(logTagTableSizes[i])); j++){
            gtable[i][j].init(numBr);
        }
    }

    uint64_t bimodalTableSize = 1ULL << logTagTableSizes[0];
    btable = new BTableEntry[bimodalTableSize];  //create base table
    for (int j = 0; j < bimodalTableSize; j++){
        btable[j].init(numBr);
    }

    usealtonnatable = new UseAltOnNaEntry[numUseAltOnNa];
    for (int j = 0; j < numUseAltOnNa; j++){
        usealtonnatable[j].init(numBr);
    }
}

TAGEBase_BLK::~TAGEBase_BLK(){
    for (unsigned i = 0; i <= nHistoryTables; ++i) {
        for (int j = 0; j < (1<<(logTagTableSizes[i])); j++){
            gtable[i][j].free();
        }
        delete []gtable[i];
    }
    delete []gtable;
    for (auto& history : threadHistory) {
        delete []history.globalHistory;
    }

    for (auto& history : threadHistory) {
        for (int i = 1; i <= nHistoryTables; i++) {
            history.computeIndices[i].free();
            history.computeTags[0][i].free();
            history.computeTags[1][i].free();
        }
        delete []history.computeIndices;
        delete []history.computeTags[0];
        delete []history.computeTags[1];
    }

    delete []tableIndices;
    delete []tableTags;
    uint64_t bimodalTableSize = 1ULL << logTagTableSizes[0];
    for (int j = 0; j < bimodalTableSize; j++){
        btable[j].free();
    }
    delete []btable;
    for (int j = 0; j < numUseAltOnNa; j++){
        usealtonnatable[j].free();
    }
    delete []usealtonnatable;

}

int
TAGEBase_BLK::bindex(Addr pc_in) const
{
    return ((pc_in >> instShiftAmt) & ((1ULL << (logTagTableSizes[0])) - 1));
}

int
TAGEBase_BLK::F(int A, int size, int bank) const
{
    int A1, A2;
    //assert(A == 0);
    A = A & ((1ULL << size) - 1);
    A1 = (A & ((1ULL << logTagTableSizes[bank]) - 1));
    A2 = (A >> logTagTableSizes[bank]);
    A2 = ((A2 << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
       + (A2 >> (logTagTableSizes[bank] - bank));
    A = A1 ^ A2;
    A = ((A << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
      + (A >> (logTagTableSizes[bank] - bank));
    //assert(A == 0);
    return (A);
}

// gindex computes a full hash of pc, ghist and pathHist
int
TAGEBase_BLK::gindex(ThreadID tid, Addr pc, int bank) const
{
    int index;
    const unsigned int shiftedPc = pc >> instShiftAmt;

    if (true){
        int hlen = (histLengths[bank] > pathHistBits) ? pathHistBits :
                                                    histLengths[bank];
        index =
            shiftedPc ^
            threadHistory[tid].computeIndices[bank].comp ^
            F(threadHistory[tid].pathHist, hlen, bank);
    }else{
        index =
            shiftedPc ^
            threadHistory[tid].computeIndices[bank].comp;
    }

    index =
        shiftedPc ^
        threadHistory[tid].computeIndices[bank].comp;

    return (index & ((1ULL << (logTagTableSizes[bank])) - 1));

}


// Tag computation
uint16_t
TAGEBase_BLK::gtag(ThreadID tid, Addr pc, int bank) const
{
    int tag = (pc >> (instShiftAmt + logTagTableSizes[bank])) ^
              threadHistory[tid].computeTags[0][bank].comp ^
              (threadHistory[tid].computeTags[1][bank].comp << 1);

    return (tag & ((1ULL << tagTableTagWidths[bank]) - 1));
}


// Up-down saturating counter
template<typename T>
void
TAGEBase_BLK::ctrUpdate(T & ctr, bool taken, int nbits)
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
template void TAGEBase_BLK::ctrUpdate(int8_t & ctr, bool taken, int nbits);
template void TAGEBase_BLK::ctrUpdate(int & ctr, bool taken, int nbits);

// Up-down unsigned saturating counter
void
TAGEBase_BLK::unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits)
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

// Bimodal prediction
bool
TAGEBase_BLK::getBimodePred(Addr pc, TageBranchInfo* bi, unsigned slot) const
{

    return (btable[bi->bimodalIndex].ctr[slot] >= 0);

}

void
TAGEBase_BLK::baseUpdate(Addr pc, bool taken, TageBranchInfo* bi,
                                        unsigned bank_idx, unsigned br_idx)
{
    ctrUpdate(btable[bi->bimodalIndex].ctr[bank_idx], taken,
                                    baseTableCounterBits);
}

// shifting the global history:  we manage the history in a big table in order
// to reduce simulation time
void
TAGEBase_BLK::updateGHist(uint8_t * &h, bool* dir, unsigned condNum,
                                                uint8_t * tab, int &pt)
{
    unsigned iterNum = condNum;
    unsigned slot = 0;
    while (iterNum > 0 ){
        if (pt == 0) {
            // DPRINTF(Tage, "Rolling over the histories\n");
            // Copy the lowest maxHist bit of globalHistoryBuffer
            // to the highest bit, such that
            // the last maxHist outcomes are still reachable
            // through pt[0 .. maxHist - 1].
            for (int i = 0; i < (maxHist + MaxNumBr); i++)
                tab[histBufferSize - (maxHist + MaxNumBr) + i] = tab[i];
            pt =  histBufferSize - (maxHist + MaxNumBr);
            h = &tab[pt];
        }
        pt--;
        h--;
        h[0] = (dir[slot]) ? 1 : 0;   //*h = (dir) ? 1 : 0;
        slot++;
        iterNum--;
    }
}

void
TAGEBase_BLK::calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                  TageBranchInfo* bi)
{
    // computes the table addresses and the partial tags
    for (int i = 1; i <= nHistoryTables; i++) {
        //calculate idx of every tage table
        tableIndices[i] = gindex(tid, branch_pc, i);
        //record current idx to ftq entry
        bi->tableIndices[i] = tableIndices[i];
        //calculate tag of every tage table
        tableTags[i] = gtag(tid, branch_pc, i);
        //record current tag to ftq entry
        bi->tableTags[i] = tableTags[i];
        DPRINTF(Tage, "table %d folded idx before pred:%d\n",
                i, tableIndices[i]);
    }
}

unsigned
TAGEBase_BLK::getUseAltIdx(TageBranchInfo* bi, Addr branch_pc)
{
    // There is only 1 counter on the base TAGE implementation
    return (branch_pc >> instShiftAmt) & (numUseAltOnNa-1);
}

void
TAGEBase_BLK::tagePredict(ThreadID tid, Addr branch_pc,
              bool* cond_branch, TageBranchInfo* bi, bool* pred_taken)
{
    bool bank_remap;
    unsigned bank_idx;
    Addr pc = branch_pc;
    if (((branch_pc >> instShiftAmt) & 1) == 1){
        bank_remap = true;
    }else{
        bank_remap = false;
    }

    // TAGE prediction
    //calculate idx and tag for all tage tables
    calculateIndicesAndTags(tid, pc, bi);
    bi->bimodalIndex = bindex(pc);
    bi->branchPC = branch_pc;

    //remap bank slot to br slot
    for (unsigned j = 0; j < numBr; j++) {
        if (cond_branch[j]) {
            bi->hitBank[j] = 0;
            bi->hitBankCtr[j] = 0;
            bi->altBank[j] = 0;
            bi->altBankCtr[j] = 0;
            if (bank_remap){
                bank_idx = !j;
            }else{
                bank_idx = j;
            }
            assert(bank_idx == 0 || bank_idx == 1);
            bi->bimodalCtr[j] = btable[bi->bimodalIndex].ctr[bank_idx];

            for (int i = nHistoryTables; i > 0; i--) {
                if (noSkip[i] &&
                    gtable[i][tableIndices[i]].slot[bank_idx].valid &&
                    gtable[i][tableIndices[i]].slot[bank_idx].tag ==
                                                        tableTags[i]) {
                    bi->hitBank[j] = i;        //the first hit table
                    bi->hitBankCtr[j] =
                                gtable[i][tableIndices[i]].slot[bank_idx].ctr;
                    bi->hitBankU[j] =
                                gtable[i][tableIndices[i]].slot[bank_idx].u;
                    bi->hitBankIndex[j] = tableIndices[bi->hitBank[j]];
                    break;
                }
            }

            //Look for the alternate bank
            for (int i = bi->hitBank[j] - 1; i > 0; i--) {
                if (noSkip[i] &&
                    gtable[i][tableIndices[i]].slot[bank_idx].valid &&
                    gtable[i][tableIndices[i]].slot[bank_idx].tag ==
                                                        tableTags[i]) {
                    bi->altBank[j] = i;
                    bi->altBankCtr[j] =
                                gtable[i][tableIndices[i]].slot[bank_idx].ctr;
                    bi->altBankIndex[j] = tableIndices[bi->altBank[j]];
                    break;
                }
            }

            //computes the prediction and the alternate prediction
            if (bi->hitBank[j] > 0) {  //if have provider
                bi->altTaken[j] = getBimodePred(pc, bi, bank_idx);
                bi->longestMatchPred[j] = bi->hitBankCtr[j] >= 0;
                //provider with low conf
                bi->pseudoNewAlloc[j] =
                    abs(2 * bi->hitBankCtr[j] + 1) <= 1;
                //if the entry is recognized as a newly allocated entry and
                //useAltPredForNewlyAllocated is positive use the alternate
                //prediction
                if ((usealtonnatable[getUseAltIdx(bi, branch_pc)].ctr[j] < 0)
                    || ! bi->pseudoNewAlloc[j]) {
                    bi->tagePred[j] = bi->longestMatchPred[j];
                    bi->provider[j] = TAGE_LONGEST_MATCH;
                    bi->hasProvider[j] = true;
                    bi->providerCtr[j] = bi->hitBankCtr[j];
                } else {
                    //if confidence is low and useAltOnNaCtrs is greater than 0
                    bi->tagePred[j] = bi->altTaken[j];
                    bi->provider[j] = bi->altBank[j] ? TAGE_ALT_MATCH
                                               : BIMODAL_ALT_MATCH;
                }
            } else {
                bi->altTaken[j] = getBimodePred(pc, bi, bank_idx);
                bi->tagePred[j] = bi->altTaken[j];
                bi->longestMatchPred[j] = bi->altTaken[j];
                bi->provider[j] = BIMODAL_ONLY;
            }
            //end TAGE prediction

            pred_taken[j] = bi->tagePred[j];
            //DPRINTF(Tage, "Predict for %lx slot %d: taken?:%d,
            //                tagePred:%d, altPred:%d\n",
            //                branch_pc, j, pred_taken[j],
            //                bi->tagePred[j], bi->altTaken[j]);
            bi->condBranch[j] = true;
        }else{
            pred_taken[j] = false;
            bi->condBranch[j] = false;
        }
    }
}

void
TAGEBase_BLK::adjustAlloc(bool & alloc, bool taken, bool pred_taken)
{
    // Nothing for this base class implementation
}

void
TAGEBase_BLK::handleAllocAndUReset(bool alloc, bool taken,
                                    TageBranchInfo* bi,
                                    int nrand, unsigned bank_idx,
                                    unsigned br_idx)
{
    if (alloc) {
        // is there some "unuseful" entry to allocate
        int numCanAlloc = 0;
        int numCannotAlloc = 0;
        // looking from last table to provider+1 table，find the smallest u bit
        for (int i = nHistoryTables; i > bi->hitBank[br_idx]; i--) {
            if (gtable[i][bi->tableIndices[i]].slot[bank_idx].valid){
                if (gtable[i][bi->tableIndices[i]].slot[bank_idx].u == 0) {
                    numCanAlloc++;
                }else{
                    numCannotAlloc++;
                }
            }else{
                numCanAlloc++;
            }
        }
        assert(numCanAlloc + numCannotAlloc ==
                nHistoryTables - bi->hitBank[br_idx]);

        if (bankTickCtrs[br_idx] < 0){
            bankTickCtrs[br_idx] = 0;
        }else if (bankTickCtrs[br_idx] > ((1 << logUResetPeriod) - 1)){
            bankTickCtrs[br_idx] = ((1 << logUResetPeriod) - 1);
        }else{
            bankTickCtrs[br_idx] = bankTickCtrs[br_idx] +
                                    numCannotAlloc - numCanAlloc;
        }

        int X = bi->hitBank[br_idx] + 1;
        if (numCanAlloc == 0) { //if all u bit greater than 0
            for (int i = X; i <= nHistoryTables; i++){
                unsignedCtrUpdate(gtable[i][bi->
                                tableIndices[i]].slot[bank_idx].u,
                                false, tagTableUBits);
            }
        }

        unsigned numAllocated = 0;

        for (int i = X; i <= nHistoryTables; i++) {
            if (!gtable[i][bi->tableIndices[i]].slot[bank_idx].valid) {
                gtable[i][bi->tableIndices[i]].slot[bank_idx].u = 0;
                gtable[i][bi->tableIndices[i]].slot[bank_idx].valid = true;
                gtable[i][bi->tableIndices[i]].slot[bank_idx].tag =
                                                        bi->tableTags[i];
                //init value when allocate
                gtable[i][bi->tableIndices[i]].slot[bank_idx].ctr =
                                                        (taken) ? 0 : -1;
                ++numAllocated;
                if (numAllocated == maxNumAlloc) {  //maxNumAlloc=1
                    break;
                }
            }
        }

        if (numAllocated == 0){
            for (int i = X; i <= nHistoryTables; i++) {
                if (gtable[i][bi->tableIndices[i]].slot[bank_idx].u == 0) {
                    gtable[i][bi->tableIndices[i]].slot[bank_idx].valid = true;
                    gtable[i][bi->tableIndices[i]].slot[bank_idx].tag =
                                                        bi->tableTags[i];
                    //init value when allocate
                    gtable[i][bi->tableIndices[i]].slot[bank_idx].ctr =
                                                        (taken) ? 0 : -1;
                    ++numAllocated;
                    if (numAllocated == maxNumAlloc) {  //maxNumAlloc=1
                        break;
                    }
                }
            }
        }

        if (numAllocated > 0){
            if (br_idx == 0){
                stats.tage_bank_0_allocate_success++;
            }else{
                stats.tage_bank_1_allocate_success++;
            }
        }else{
            DPRINTF(Tage, "allocate tage table fail: slot: %d\n", br_idx);
            if (br_idx == 0){
                stats.tage_bank_0_allocate_failure++;
            }else{
                stats.tage_bank_1_allocate_failure++;
            }
        }

        handleUReset(bank_idx, br_idx); //clear u bit periodic
    }

    tCounter++;
}

void
TAGEBase_BLK::handleUReset(unsigned bank_idx, unsigned br_idx)
{
    //periodic reset of u: reset is not complete but bit by bit
    if (bankTickCtrs[br_idx] >= ((1 << logUResetPeriod) - 1)) {
        if (br_idx == 0){
            stats.tage_bank_0_reset_u++;
        }else{
            stats.tage_bank_1_reset_u++;
        }
        for (int i = 1; i <= nHistoryTables; i++) {
            for (int j = 0; j < (1 << logTagTableSizes[i]); j++) {
                resetUctr(gtable[i][j].slot[bank_idx].u);
            }
        }
        bankTickCtrs[br_idx] = 0;
    }
}

void
TAGEBase_BLK::resetUctr(uint8_t & u)
{
    //u >>= 1;
    u = 0;
}

void
TAGEBase_BLK::condBranchUpdate(ThreadID tid, Addr branch_pc,
                                bool* taken, bool* isCond,
                                TageBranchInfo* bi, int nrand)
{
    // TAGE UPDATE
    if (taken[0]){
        assert(!isCond[1]);
    }
    bool alloc;
    bool PseudoNewAlloc;
    bool bank_remap;
    unsigned bank_idx;
    if (((branch_pc >> instShiftAmt) & 1) == 1){
        bank_remap = true;
    }else{
        bank_remap = false;
    }
    for (int i = 1; i <= nHistoryTables; i++){
        DPRINTF(Tage, "commit block for tage table: %d, idx: %d\n",
                i, bi->tableIndices[i]);
    }

    for (unsigned j = 0; j < numBr; j++){
        if (isCond[j]) {
            if (bank_remap){
                bank_idx = !j;
            }else{
                bank_idx = j;
            }
            // try to allocate a  new entries only if prediction was wrong
            alloc = (bi->tagePred[j] != taken[j]) &&
                        (bi->hitBank[j] < nHistoryTables);
            if (bi->hitBank[j] > 0) {
                // Manage the selection between longest matching and alternate
                // matching for "pseudo"-newly allocated longest matching entry
                PseudoNewAlloc = bi->pseudoNewAlloc[j];
                // an entry is considered as newly allocated if its prediction
                // confidence is weak
                if (PseudoNewAlloc) {
                    //if alt is diff with provider
                    if (bi->tagePred[j] != bi->altTaken[j] &&
                            bi->provider[j] == TAGE_LONGEST_MATCH) {
                        ctrUpdate(           //update useAltOnNaCtrs table
                        usealtonnatable[getUseAltIdx(bi, branch_pc)].ctr[j],
                        bi->altTaken[j] == taken[j], useAltOnNaBits);
                    }
                }
            }

            /** allocate tage table and reset u bit */
            handleAllocAndUReset(alloc, taken[j], bi, nrand, bank_idx, j);

            handleTAGEUpdate(branch_pc, taken[j], bi, bank_idx, j);
        }
    }

}

void
TAGEBase_BLK::handleTAGEUpdate(Addr branch_pc,
                            bool taken, TageBranchInfo* bi,
                            unsigned bank_idx, unsigned br_idx)
{
    if (bi->hitBank[br_idx] > 0) {  //if has provider
        //update ctr in provider table
        ctrUpdate(gtable[bi->hitBank[br_idx]][bi->hitBankIndex[br_idx]].
                    slot[bank_idx].ctr,
                    taken,  tagTableCounterBits);

        assert(bi->hitBank[br_idx] <= nHistoryTables);

        DPRINTF(Tage, "update tage table: %d, slot: %d\n",
                    bi->hitBank[br_idx], br_idx);
        if (bi->provider[br_idx] != TAGE_LONGEST_MATCH){
            baseUpdate(branch_pc, taken, bi, bank_idx, br_idx);
        }

        // update the u counter
        if (bi->tagePred[br_idx] != bi->altTaken[br_idx]) {
            unsignedCtrUpdate(gtable[bi->hitBank[br_idx]]
                              [bi->hitBankIndex[br_idx]].
                              slot[bank_idx].u,
                              bi->tagePred[br_idx] == taken, tagTableUBits);
        }
    } else {  //update base table
        baseUpdate(branch_pc, taken, bi, bank_idx, br_idx);
    }
}

void
TAGEBase_BLK::updateHistories(ThreadID tid, Addr branch_pc,
                              bool* taken, bool* isCond,
                              TageBranchInfo* bi, bool speculative,
                              Addr target)
{
    if (speculative != speculativeHistUpdate) {
        return;
    }
    ThreadHistory& tHist = threadHistory[tid];
    //  UPDATE HISTORIES
    bool pathbit = ((branch_pc >> instShiftAmt) & 1);
    //on a squash, return pointers to this and recompute indices.
    //update user history
    unsigned condNum = 0;
    for (int i = 0; i < numBr; i++){
        condNum = condNum + isCond[i];
        if (taken[i]){
            break;
        }
    }

    if (speculative) {
        bi->ptGhist = tHist.ptGhist;
        bi->pathHist = tHist.pathHist;
        for (int i = 120; i >= 0; i--){
            bi->ghr121[i] = tHist.gHist[i];
        }
    }

    if (condNum > 0){
        updateGHist(tHist.gHist, taken, condNum,
                tHist.globalHistory, tHist.ptGhist);
    }
    tHist.pathHist = (tHist.pathHist << 1) + pathbit; //update path history
    tHist.pathHist = (tHist.pathHist & ((1ULL << pathHistBits) - 1));

    DPRINTF(Tage, "tage pred dir: %d, %d; condNum: %d\n",
            taken[0], taken[1], condNum);

    for (int i = 1; i <= nHistoryTables; i++)
    {
        if (speculative) {
            //record snapshot of folded history
            bi->ci[i]  = tHist.computeIndices[i].comp;
            bi->ct0[i] = tHist.computeTags[0][i].comp;
            bi->ct1[i] = tHist.computeTags[1][i].comp;
            DPRINTF(Tage, "table %d folded history before pred:%d\n",
                        i, threadHistory[0].computeIndices[i].comp);
        }
        //update all folded history
        if (condNum > 0){
            tHist.computeIndices[i].update(tHist.gHist, condNum);
            tHist.computeTags[0][i].update(tHist.gHist, condNum);
            tHist.computeTags[1][i].update(tHist.gHist, condNum);
            tHist.computeIndices[i].check_folded_history(tHist.gHist);
            tHist.computeTags[0][i].check_folded_history(tHist.gHist);
            tHist.computeTags[1][i].check_folded_history(tHist.gHist);
        }

    }
    // DPRINTF(Tage, "Updating global histories with branch:%lx;
    //         taken?:%d, "
    //         "path Hist: %x; pointer:%d\n",
    //         branch_pc, taken, tHist.pathHist,
    //         tHist.ptGhist);
    assert(threadHistory[tid].gHist ==
        &threadHistory[tid].globalHistory[threadHistory[tid].ptGhist]);
}

//when flush，recover global history
void
TAGEBase_BLK::squash(ThreadID tid, bool taken,
                        TageBranchInfo *bi, bool isCondMisp,
                        unsigned numBrBefore)
{
    if (!speculativeHistUpdate) {
        /* If there are no speculative updates, no actions are needed */
        return;
    }

    ThreadHistory& tHist = threadHistory[tid];
    bool taken_recover[numBr];
    unsigned num_recover_br;
    // DPRINTF(Tage, "Restoring branch info: %lx; taken? %d; PathHistory:%x, "
    //         "pointer:%d\n", bi->branchPC, taken, bi->pathHist, bi->ptGhist);
    tHist.pathHist = ((bi->pathHist << 1) +
                        ((bi->branchPC >> instShiftAmt) & 1)) &
                        ((1ULL << pathHistBits) - 1);  //recover path history
    tHist.ptGhist = bi->ptGhist;    //recover ptGhist of global history
    tHist.gHist = &(tHist.globalHistory[tHist.ptGhist]);
    if (isCondMisp){
        //tHist.gHist[0] = (taken ? 1 : 0);
        if (numBrBefore == 0){
            taken_recover[0] = taken;
            taken_recover[1] = false;
            num_recover_br = 1;
        }else if (numBrBefore == 1){
            taken_recover[0] = false;
            taken_recover[1] = taken;
            num_recover_br = 2;
        }else{
            assert(false);
        }
        //update global history after recover
        updateGHist(tHist.gHist, &taken_recover[0], num_recover_br,
                tHist.globalHistory, tHist.ptGhist);
        //recover folded history and update
        for (int i = 1; i <= nHistoryTables; i++) {
            tHist.computeIndices[i].comp = bi->ci[i];
            tHist.computeTags[0][i].comp = bi->ct0[i];
            tHist.computeTags[1][i].comp = bi->ct1[i];
            tHist.computeIndices[i].update(tHist.gHist, num_recover_br);
            tHist.computeTags[0][i].update(tHist.gHist, num_recover_br);
            tHist.computeTags[1][i].update(tHist.gHist, num_recover_br);
            DPRINTF(Tage, "ghr squash folded index before recover: %d\n",
                    bi->ci[i]);
            DPRINTF(Tage, "ghr squash recover cond number: %d\n",
                    num_recover_br);
            tHist.computeIndices[i].check_folded_history(tHist.gHist);
            tHist.computeTags[0][i].check_folded_history(tHist.gHist);
            tHist.computeTags[1][i].check_folded_history(tHist.gHist);
        }
    }else{
        if (numBrBefore == 0){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 0;
        }else if (numBrBefore == 1){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 1;
        }else if (numBrBefore == 2){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 2;
        }else{
            assert(false);
        }
        //update global history after recover
        if (num_recover_br > 0){
            updateGHist(tHist.gHist, &taken_recover[0], num_recover_br,
                    tHist.globalHistory, tHist.ptGhist);
        }
        //recover folded history
        for (int i = 1; i <= nHistoryTables; i++) {
            tHist.computeIndices[i].comp = bi->ci[i];
            tHist.computeTags[0][i].comp = bi->ct0[i];
            tHist.computeTags[1][i].comp = bi->ct1[i];
            if (num_recover_br > 0){
                tHist.computeIndices[i].update(tHist.gHist, num_recover_br);
                tHist.computeTags[0][i].update(tHist.gHist, num_recover_br);
                tHist.computeTags[1][i].update(tHist.gHist, num_recover_br);
            }
            tHist.computeIndices[i].check_folded_history(tHist.gHist);
            tHist.computeTags[0][i].check_folded_history(tHist.gHist);
            tHist.computeTags[1][i].check_folded_history(tHist.gHist);
        }
    }
}

void
TAGEBase_BLK::extraAltCalc(TageBranchInfo* bi)
{
    // do nothing. This is only used in some derived classes
    return;
}

void
TAGEBase_BLK::updateStats(bool* taken, bool* isCond, TageBranchInfo* bi)
{
    if (isCond[0]){
        stats.tage_bank_0_has_update++;
        if (bi->provider[0] != TAGE_LONGEST_MATCH){
            stats.tage_bank_0_use_alt_pred++;
            if (bi->tagePred[0] == taken[0]){
                stats.tage_bank_0_alt_correct++;
            }else{
                stats.tage_bank_0_alt_wrong++;
            }
        }else{
            stats.tage_bank_0_use_provider_pred++;
            if (bi->tagePred[0] == taken[0]){
                stats.tage_bank_0_use_provider_correct++;
            }else{
                stats.tage_bank_0_use_provider_wrong++;
            }
        }

        if (bi->tagePred[0] != taken[0]){
            stats.tage_bank_0_mispred++;
        }
        if (bi->hitBank[0] > 0 && bi->provider[0] != TAGE_LONGEST_MATCH){
            stats.tage_bank_0_na++;
        }
        if (bi->hitBank[0] > 0){
            stats.tage_bank_0_use_provider_hit++;
            DPRINTF(Tage, "slot0 hit tage table:%d\n", bi->hitBank[0]);
        }else{
            DPRINTF(Tage, "slot0 miss tage table, tage pred:%d\n",
                    bi->tagePred[0]);
        }
    }

    if (isCond[1]){
        stats.tage_bank_1_has_update++;
        if (bi->provider[1] != TAGE_LONGEST_MATCH){
            stats.tage_bank_1_use_alt_pred++;
            if (bi->tagePred[1] == taken[1]){
                stats.tage_bank_1_alt_correct++;
            }else{
                stats.tage_bank_1_alt_wrong++;
            }
        }else{
            stats.tage_bank_1_use_provider_pred++;
            if (bi->tagePred[1] == taken[1]){
                stats.tage_bank_1_use_provider_correct++;
            }else{
                stats.tage_bank_1_use_provider_wrong++;
            }
        }

        if (bi->tagePred[1] != taken[1]){
            stats.tage_bank_1_mispred++;
        }
        if (bi->hitBank[1] > 0 && bi->provider[1] != TAGE_LONGEST_MATCH){
            stats.tage_bank_1_na++;
        }
        if (bi->hitBank[1] > 0){
            stats.tage_bank_1_use_provider_hit++;
            DPRINTF(Tage, "slot1 hit tage table:%d\n", bi->hitBank[1]);
        }else{
            DPRINTF(Tage, "slot1 miss tage table, tage pred:%d\n",
                            bi->tagePred[1]);
        }
    }


}

unsigned
TAGEBase_BLK::getGHR(ThreadID tid, TageBranchInfo *bi) const
{
    unsigned val = 0;
    for (unsigned i = 0; i < 32; i++) {
        // Make sure we don't go out of bounds
        int gh_offset = bi->ptGhist + i;
        assert(&(threadHistory[tid].globalHistory[gh_offset]) <
               threadHistory[tid].globalHistory + histBufferSize);
        val |=
            ((threadHistory[tid].globalHistory[gh_offset] & 0x1) << i);
    }

    return val;
}

TAGEBase_BLK::TAGEBaseBLKStats::TAGEBaseBLKStats(
    statistics::Group *parent, unsigned nHistoryTables)
    : statistics::Group(parent),
      ADD_STAT(tage_bank_0_mispred,
                statistics::units::Count::get(),
                        "tage_bank_0_mispred"),
      ADD_STAT(tage_bank_1_mispred,
                statistics::units::Count::get(),
                        "tage_bank_1_mispred"),
      ADD_STAT(tage_bank_0_reset_u,
                statistics::units::Count::get(),
                        "tage_bank_0_reset_u"),
      ADD_STAT(tage_bank_1_reset_u,
                statistics::units::Count::get(),
                        "tage_bank_1_reset_u"),
      ADD_STAT(tage_bank_0_has_update,
                statistics::units::Count::get(),
                        "tage_bank_0_has_update"),
      ADD_STAT(tage_bank_1_has_update,
                statistics::units::Count::get(),
                        "tage_bank_1_has_update"),
      ADD_STAT(tage_bank_0_use_provider_hit,
                statistics::units::Count::get(),
                        "tage_bank_0_use_provider_hit"),
      ADD_STAT(tage_bank_1_use_provider_hit,
                statistics::units::Count::get(),
                        "tage_bank_1_use_provider_hit"),
      ADD_STAT(tage_bank_0_allocate_success,
                statistics::units::Count::get(),
                        "tage_bank_0_allocate_success"),
      ADD_STAT(tage_bank_1_allocate_success,
                statistics::units::Count::get(),
                        "tage_bank_1_allocate_success"),
      ADD_STAT(tage_bank_0_allocate_failure,
                statistics::units::Count::get(),
                        "tage_bank_0_allocate_failure"),
      ADD_STAT(tage_bank_1_allocate_failure,
                statistics::units::Count::get(),
                        "tage_bank_1_allocate_failure"),
      ADD_STAT(tage_bank_0_use_provider_pred,
                statistics::units::Count::get(),
                        "tage_bank_0_use_provider_pred"),
      ADD_STAT(tage_bank_1_use_provider_pred,
                statistics::units::Count::get(),
                        "tage_bank_1_use_provider_pred"),
      ADD_STAT(tage_bank_0_use_provider_correct,
                statistics::units::Count::get(),
                        "tage_bank_0_use_provider_correct"),
      ADD_STAT(tage_bank_1_use_provider_correct,
                statistics::units::Count::get(),
                        "tage_bank_1_use_provider_correct"),
      ADD_STAT(tage_bank_0_use_provider_wrong,
                statistics::units::Count::get(),
                        "tage_bank_0_use_provider_wrong"),
      ADD_STAT(tage_bank_1_use_provider_wrong,
                statistics::units::Count::get(),
                        "tage_bank_1_use_provider_wrong"),
      ADD_STAT(tage_bank_0_use_alt_pred,
                statistics::units::Count::get(),
                        "tage_bank_0_use_alt_pred"),
      ADD_STAT(tage_bank_1_use_alt_pred,
                statistics::units::Count::get(),
                        "tage_bank_1_use_alt_pred"),
      ADD_STAT(tage_bank_0_alt_correct,
                statistics::units::Count::get(),
                        "tage_bank_0_alt_correct"),
      ADD_STAT(tage_bank_1_alt_correct,
                statistics::units::Count::get(),
                        "tage_bank_1_alt_correct"),
      ADD_STAT(tage_bank_0_alt_wrong,
                statistics::units::Count::get(),
                        "tage_bank_0_alt_wrong"),
      ADD_STAT(tage_bank_1_alt_wrong,
                statistics::units::Count::get(),
                        "tage_bank_1_alt_wrong"),
      ADD_STAT(tage_bank_0_alt_differs,
                statistics::units::Count::get(),
                        "tage_bank_0_alt_differs"),
      ADD_STAT(tage_bank_1_alt_differs,
                statistics::units::Count::get(),
                        "tage_bank_1_alt_differs"),
      ADD_STAT(tage_bank_0_use_alt_on_na_ctr_updated,
                statistics::units::Count::get(),
                        "tage_bank_0_use_alt_on_na_ctr_updated"),
      ADD_STAT(tage_bank_1_use_alt_on_na_ctr_updated,
                statistics::units::Count::get(),
                        "tage_bank_1_use_alt_on_na_ctr_updated"),
      ADD_STAT(tage_bank_0_use_alt_on_na_ctr_inc,
                statistics::units::Count::get(),
                        "tage_bank_0_use_alt_on_na_ctr_inc"),
      ADD_STAT(tage_bank_1_use_alt_on_na_ctr_inc,
                statistics::units::Count::get(),
                        "tage_bank_1_use_alt_on_na_ctr_inc"),
      ADD_STAT(tage_bank_0_use_alt_on_na_ctr_dec,
                statistics::units::Count::get(),
                        "tage_bank_0_use_alt_on_na_ctr_dec"),
      ADD_STAT(tage_bank_1_use_alt_on_na_ctr_dec,
                statistics::units::Count::get(),
                        "tage_bank_1_use_alt_on_na_ctr_dec"),
      ADD_STAT(tage_bank_0_na,
                statistics::units::Count::get(),
                        "tage_bank_0_na"),
      ADD_STAT(tage_bank_1_na,
                statistics::units::Count::get(),
                        "tage_bank_1_na"),
      ADD_STAT(tage_bank_0_use_na_correct,
                statistics::units::Count::get(),
                        "tage_bank_0_use_na_correct"),
      ADD_STAT(tage_bank_1_use_na_correct,
                statistics::units::Count::get(),
                        "tage_bank_1_use_na_correct"),
      ADD_STAT(tage_bank_0_use_na_wrong,
                statistics::units::Count::get(),
                        "tage_bank_0_use_na_wrong"),
      ADD_STAT(tage_bank_1_use_na_wrong,
                statistics::units::Count::get(),
                        "tage_bank_1_use_na_wrong")
{

}

unsigned
TAGEBase_BLK::getTageCtrBits() const
{
    return tagTableCounterBits;
}

int
TAGEBase_BLK::getPathHist(ThreadID tid) const
{
    return threadHistory[tid].pathHist;
}

bool
TAGEBase_BLK::isSpeculativeUpdateEnabled() const
{
    return speculativeHistUpdate;
}

size_t
TAGEBase_BLK::getSizeInBits() const {
    size_t bits = 0;
    for (int i = 1; i <= nHistoryTables; i++) {
        bits += (1 << logTagTableSizes[i]) *
            (tagTableCounterBits + tagTableUBits +
                                tagTableTagWidths[i]);
    }
    uint64_t bimodalTableSize = 1ULL << logTagTableSizes[0];
    bits += numUseAltOnNa * useAltOnNaBits;
    bits += bimodalTableSize;
    bits += (bimodalTableSize >> logRatioBiModalHystEntries);
    bits += histLengths[nHistoryTables];
    bits += pathHistBits;
    bits += logUResetPeriod;
    return bits;
}

TAGEBase_BLK::TageBranchInfo*
TAGEBase_BLK::makeBranchInfo() {
    return new TageBranchInfo(*this);
}

} // namespace branch_prediction
} // namespace gem5

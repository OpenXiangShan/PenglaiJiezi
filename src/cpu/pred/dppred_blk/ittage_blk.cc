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
 * Implementation of a ITTAGE branch predictor
 */
#include "cpu/pred/dppred_blk/ittage_blk.hh"

#include <queue>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "debug/ITTage.hh"

namespace gem5
{

namespace branch_prediction
{

ITTAGE_BLK::ITTAGE_BLK(const ITTAGE_BLKParams &p)
   : SimObject(p),
     nHistoryTables(p.nHistoryTables),
     tagTableCounterBits(p.tagTableCounterBits),
     tagTableUBits(p.tagTableUBits),
     histBufferSize(p.histBufferSize),
     minHist(p.histLengths[1]),
     maxHist(p.histLengths[p.nHistoryTables]),
     histLengths(p.histLengths),
     pathHistBits(p.pathHistBits),
     tagTableTagWidths(p.tagTableTagWidths),
     logTagTableSizes(p.logTagTableSizes),
     //use construct func to create a vector of p.numThreads entries
     threadHistory(p.numThreads),
     logUResetPeriod(p.logUResetPeriod),
     initialTCounterValue(p.initialTCounterValue),
     maxNumAlloc(p.maxNumAlloc),
     noSkip(p.noSkip),
     speculativeHistUpdate(p.speculativeHistUpdate),
     instShiftAmt(p.instShiftAmt),
     initialized(false),
     stats(this, nHistoryTables)
{
    if (noSkip.empty()) {
        // Set all the table to enabled by default
        noSkip.resize(nHistoryTables+1, true);
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
    bankTickCtrs = 0;

    assert(histBufferSize > maxHist * 2);
    //initialize global history of all threads
    for (auto& history : threadHistory) {
        history.pathHist = 0;
        //initialize global history buffer
        history.globalHistory = new uint8_t[histBufferSize];
        //set pointer of gHist to the head of global history buffer
        history.gHist = history.globalHistory;
        //set values in global history buffer to 0
        memset(history.gHist, 0, histBufferSize);
        //set index of ptGhist to 0
        history.ptGhist = 0;
    }

    assert(tagTableTagWidths.size() == nHistoryTables+1);
    assert(logTagTableSizes.size() == nHistoryTables+1);

    // First entry is for the Bimodal table and it is untagged in this
    // implementation
    assert(tagTableTagWidths[0] == 0);

    for (auto& history : threadHistory) {
        history.computeIndices = new FoldedHistory[nHistoryTables+1];
        history.computeTags[0] = new FoldedHistory[nHistoryTables+1];
        history.computeTags[1] = new FoldedHistory[nHistoryTables+1];

        initFoldedHistories(history);  //init all params in folded history
    }
    gtable = new ITTageEntry *[nHistoryTables+1];
    buildTageTables();
    tableIndices = new int [nHistoryTables+1];
    tableTags = new int [nHistoryTables+1];
}



void
ITTAGE_BLK::initFoldedHistories(ThreadHistory & history)
{
    for (int i = 1; i <= nHistoryTables; i++) {
        history.computeIndices[i].init(
            histLengths[i], (logTagTableSizes[i]));
        history.computeTags[0][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]);
        history.computeTags[1][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]-1);
        DPRINTF(ITTage, "HistLength:%d, TTSize:%d, TTTWidth:%d\n",
                histLengths[i], logTagTableSizes[i], tagTableTagWidths[i]);
    }
}

void
ITTAGE_BLK::buildTageTables()
{
    for (int i = 1; i <= nHistoryTables; i++) {
        //create ittage tables
        gtable[i] = new ITTageEntry[1<<(logTagTableSizes[i])];
    }
}

ITTAGE_BLK::~ITTAGE_BLK(){
    for (unsigned i = 0; i <= nHistoryTables; ++i) {
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
}

int
ITTAGE_BLK::bindex(Addr pc_in) const
{
    return ((pc_in >> instShiftAmt) & ((1ULL << (logTagTableSizes[0])) - 1));
}

int
ITTAGE_BLK::F(int A, int size, int bank) const
{
    int A1, A2;

    A = A & ((1ULL << size) - 1);
    A1 = (A & ((1ULL << logTagTableSizes[bank]) - 1));
    A2 = (A >> logTagTableSizes[bank]);
    A2 = ((A2 << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
       + (A2 >> (logTagTableSizes[bank] - bank));
    A = A1 ^ A2;
    A = ((A << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
      + (A >> (logTagTableSizes[bank] - bank));
    return (A);
}

// gindex computes a full hash of pc, ghist and pathHist
int
ITTAGE_BLK::gindex(ThreadID tid, Addr pc, int bank) const
{
    int index;
    const unsigned int shiftedPc = pc >> instShiftAmt;
    index =
        shiftedPc ^
        threadHistory[tid].computeIndices[bank].comp;

    return (index & ((1ULL << (logTagTableSizes[bank])) - 1));

}


// Tag computation
uint16_t
ITTAGE_BLK::gtag(ThreadID tid, Addr pc, int bank) const
{
    int tag = (pc >> (instShiftAmt + logTagTableSizes[bank])) ^
              threadHistory[tid].computeTags[0][bank].comp ^
              (threadHistory[tid].computeTags[1][bank].comp << 1);

    return (tag & ((1ULL << tagTableTagWidths[bank]) - 1));
}


// Up-down saturating counter
template<typename T>
void
ITTAGE_BLK::ctrUpdate(T & ctr, bool taken, int nbits)
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
template void ITTAGE_BLK::ctrUpdate(int8_t & ctr, bool taken, int nbits);
template void ITTAGE_BLK::ctrUpdate(int & ctr, bool taken, int nbits);

// Up-down unsigned saturating counter
void
ITTAGE_BLK::unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits)
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


// shifting the global history:  we manage the history in a big table in order
// to reduce simulation time
void
ITTAGE_BLK::updateGHist(uint8_t * &h, bool* dir, unsigned condNum,
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
ITTAGE_BLK::calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                  ITTageBranchInfo* bi)
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
        DPRINTF(ITTage, "table %d folded idx before pred:%d\n",
                i, tableIndices[i]);
    }
}


Addr
ITTAGE_BLK::predict(ThreadID tid, Addr branch_pc, bool* pred_taken,
                    bool* pred_isCond, ITTageBranchInfo* &bi,
                    Addr ftb_target, bool *confidence)
{
    bool finalAltPred;
    Addr pc = branch_pc;

    bi = new ITTageBranchInfo(*this);
    // TAGE prediction
    //calc idx and tag for every tage table
    calculateIndicesAndTags(tid, pc, bi);
    bi->branchPC = branch_pc;
    bi->hitBank = 0;
    bi->hitBankCtr = 0;
    bi->altBank = 0;
    bi->altBankCtr = 0;

    //Look for the bank with longest matching history
    for (int i = nHistoryTables; i > 0; i--) {
        if (noSkip[i] &&
            gtable[i][tableIndices[i]].valid &&
            gtable[i][tableIndices[i]].tag == tableTags[i]) {
            bi->hitBank = i;        //the first hit table
            bi->hitBankCtr = gtable[i][tableIndices[i]].ctr;
            bi->hitBankIndex = tableIndices[bi->hitBank];
            break;
        }
    }

    //Look for the alternate bank
    for (int i = bi->hitBank - 1; i > 0; i--) {
        if (noSkip[i] &&
            gtable[i][tableIndices[i]].valid &&
            gtable[i][tableIndices[i]].tag == tableTags[i]) {
            bi->altBank = i;
            bi->altBankCtr = gtable[i][tableIndices[i]].ctr;
            bi->altBankIndex = tableIndices[bi->altBank];
            break;
        }
    }

    //computes the prediction and the alternate prediction
    if (bi->hitBank > 0) {  //if have provider
        bi->hitBankTarget =
                    gtable[bi->hitBank][tableIndices[bi->hitBank]].target;
        if (bi->altBank > 0) {  //if alt hit
            bi->altBankTarget =
                    gtable[bi->altBank][tableIndices[bi->altBank]].target;
        }else {
            bi->altBankTarget = ftb_target;
        }

        bi->pseudoNewAlloc = bi->hitBankCtr == -2;  //provider with low conf
        if (! bi->pseudoNewAlloc) {
            bi->ittagePredTarget = bi->hitBankTarget;
            bi->taken = (bi->hitBankCtr >= 0);
            bi->provider = TAGE_LONGEST_MATCH;
            // if (bi->hitBankCtr >= 0){
            //     confidence[1] = true;
            // }
            DPRINTF(ITTage, "ittage pred provider ctr:%d\n",
                bi->hitBankCtr);
        } else {
            bi->ittagePredTarget = bi->altBank ? bi->altBankTarget
                                                    : ftb_target;
            bi->taken = bi->altBank ? (bi->altBankCtr >= 0)
                                        : false;
            bi->provider = bi->altBank ? TAGE_ALT_MATCH
                                               : BIMODAL_ALT_MATCH;
            DPRINTF(ITTage, "ittage pred provider ctr:%d\n",
                bi->hitBankCtr);
            if (bi->altBank > 0){
                DPRINTF(ITTage, "ittage pred alt ctr:%d\n",
                    bi->altBankCtr);
            }
        }
    } else {  //if not hit
        bi->ittagePredTarget = ftb_target;
        bi->taken = true;
        bi->provider = BIMODAL_ONLY;
    }

    if (bi->altBank > 0){
        assert(bi->hitBank > 0);
        finalAltPred = (bi->altBankCtr >= 0);
    }else{
        finalAltPred = true;
    }

    bi->altDiffers = bi->taken != finalAltPred;
    bi->finalPredTarget = bi->taken ? bi->ittagePredTarget :
                                      ftb_target;
    return bi->finalPredTarget;
}

void
ITTAGE_BLK::handleAllocAndUReset(bool alloc, Addr target,
                                    ITTageBranchInfo* bi)
{
    if (alloc) {
        // is there some "unuseful" entry to allocate
        int numCanAlloc = 0;
        int numCannotAlloc = 0;
        // looking from last table to provider+1 table，find the smallest u bit
        for (int i = nHistoryTables; i > bi->hitBank; i--) {
            if (gtable[i][bi->tableIndices[i]].valid){
                if (gtable[i][bi->tableIndices[i]].u == 0) {
                    numCanAlloc++;
                }else{
                    numCannotAlloc++;
                }
            }else{
                numCanAlloc++;
            }
        }
        assert(numCanAlloc + numCannotAlloc ==
                nHistoryTables - bi->hitBank);

        if (bankTickCtrs < 0){
            bankTickCtrs = 0;
        }else if (bankTickCtrs > ((1 << logUResetPeriod) - 1)){
            bankTickCtrs = ((1 << logUResetPeriod) - 1);
        }else{
            bankTickCtrs = bankTickCtrs +
                                    numCannotAlloc - numCanAlloc;
        }

        int X = bi->hitBank + 1;
        unsigned numAllocated = 0;

        for (int i = X; i <= nHistoryTables; i++) {
            if (!gtable[i][bi->tableIndices[i]].valid) {
                gtable[i][bi->tableIndices[i]].u = 0;
                gtable[i][bi->tableIndices[i]].valid = true;
                gtable[i][bi->tableIndices[i]].tag = bi->tableTags[i];
                //init value when allocate
                gtable[i][bi->tableIndices[i]].ctr = 0;
                gtable[i][bi->tableIndices[i]].target = target;
                ++numAllocated;
                if (numAllocated == maxNumAlloc) {  //maxNumAlloc=1
                    break;
                }
            }
        }

        if (numAllocated == 0){
            for (int i = X; i <= nHistoryTables; i++) {
                if (gtable[i][bi->tableIndices[i]].u == 0) {
                    gtable[i][bi->tableIndices[i]].valid = true;
                    gtable[i][bi->tableIndices[i]].tag = bi->tableTags[i];
                    //init value when allocate
                    gtable[i][bi->tableIndices[i]].ctr = 0;
                    gtable[i][bi->tableIndices[i]].target = target;
                    ++numAllocated;
                    if (numAllocated == maxNumAlloc) {  //maxNumAlloc=1
                        break;
                    }
                }
            }
        }

        if (numCanAlloc == 0) { //if all u bit greater than 0
            for (int i = X; i <= nHistoryTables; i++){
                unsignedCtrUpdate(gtable[i][bi->
                                tableIndices[i]].u,
                                false, tagTableUBits);
            }
        }

        if (numAllocated > 0){
            stats.ittage_allocate_success++;
        }else{
            stats.ittage_allocate_failure++;
        }
    }
    handleUReset(); //clear u bit periodic
    tCounter++;
}

void
ITTAGE_BLK::handleUReset()
{
    //periodic reset of u: reset is not complete but bit by bit
    if (bankTickCtrs >= ((1 << logUResetPeriod) - 1)) {
        stats.ittage_reset_u++;
        for (int i = 1; i <= nHistoryTables; i++) {
            for (int j = 0; j < (1 << logTagTableSizes[i]); j++) {
                resetUctr(gtable[i][j].u);
            }
        }
        bankTickCtrs = 0;
    }
}

void
ITTAGE_BLK::resetUctr(uint8_t & u)
{
    //u >>= 1;
    u = 0;
}

void
ITTAGE_BLK::update(ThreadID tid, Addr branch_pc,
                        Addr target, bool isIndirect,
                        ITTageBranchInfo* bi, bool &press)
{
    // ITTAGE UPDATE
    bool alloc;
    for (int i = 1; i <= nHistoryTables; i++){
        DPRINTF(ITTage, "commit block for ittage table: %d, idx: %d\n",
                i, bi->tableIndices[i]);
    }

    if (isIndirect) { //if the committed instr is cond br
        ++stats.ittage_has_update;
        if (bi->hitBank > 0 && !bi->pseudoNewAlloc){
            ++stats.ittage_use_provider_pred;
        }
        if (bi->hitBank > 0 && bi->pseudoNewAlloc){
            ++stats.ittage_use_alt_pred;
        }
        // try to allocate a new entries only if prediction was wrong
        // and provider is not the longest table
        alloc = (bi->finalPredTarget != target) &&
                /*!(bi->hitBank > 0 && target == bi->hitBankTarget
                && bi->pseudoNewAlloc) &&*/
                (bi->hitBank < nHistoryTables);

        /** allocate tage table and reset u bit */
        handleAllocAndUReset(alloc, target, bi);
        press = alloc;

        handleTAGEUpdate(branch_pc, target, bi, press);
    }
}

void
ITTAGE_BLK::handleTAGEUpdate(Addr branch_pc, Addr target,
                            ITTageBranchInfo* bi, bool &press)
{
    if (bi->hitBank > 0) {  //if has provider
        if (gtable[bi->hitBank][bi->hitBankIndex].ctr == -2){
            gtable[bi->hitBank][bi->hitBankIndex].target = target;
        }

        //update ctr in provider table
        ctrUpdate(gtable[bi->hitBank][bi->hitBankIndex].ctr,
                target == bi->hitBankTarget,  tagTableCounterBits);
        DPRINTF(ITTage, "ittage update provider ctr:%d\n",
                gtable[bi->hitBank][bi->hitBankIndex].ctr);

        //update u bit in provider table
        if (bi->altDiffers){
            unsignedCtrUpdate(gtable[bi->hitBank][bi->hitBankIndex].u,
                         bi->finalPredTarget == target, tagTableUBits);
        }

        //update ctr in alt table
        if (bi->altBank > 0 && bi->pseudoNewAlloc &&
                bi->finalPredTarget != target){
            assert(bi->altBank < 6);
            if (gtable[bi->altBank][bi->altBankIndex].ctr == -2){
                gtable[bi->altBank][bi->altBankIndex].target = target;
            }
            ctrUpdate(gtable[bi->altBank][bi->altBankIndex].ctr,
                        false,  tagTableCounterBits);
        }
        press = true;
    }
}

void
ITTAGE_BLK::updateHistories(ThreadID tid, Addr branch_pc,
                              bool* taken, bool* isCond,
                              ITTageBranchInfo* bi, bool speculative,
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
    for (int i = 0; i < MaxNumBr; i++){
        condNum = condNum + isCond[i];
        if (taken[i]){
            break;
        }
    }

    if (speculative) {
        bi->ptGhist = tHist.ptGhist; //record snapshot of ptGhist
        bi->pathHist = tHist.pathHist;
        for (int i = 120; i >= 0; i--){
            bi->ghr121[i] = tHist.gHist[i];
        }
    }

    if (condNum > 0){
        updateGHist(tHist.gHist, taken, condNum,
                tHist.globalHistory, tHist.ptGhist);
    }
    tHist.pathHist = (tHist.pathHist << 1) + pathbit; //record path history
    tHist.pathHist = (tHist.pathHist & ((1ULL << pathHistBits) - 1));

    DPRINTF(ITTage, "tage pred dir: %d, %d; condNum: %d\n",
            taken[0], taken[1], condNum);

    for (int i = 1; i <= nHistoryTables; i++)
    {
        if (speculative) {
            //record snapshot of folded history
            bi->ci[i]  = tHist.computeIndices[i].comp;
            bi->ct0[i] = tHist.computeTags[0][i].comp;
            bi->ct1[i] = tHist.computeTags[1][i].comp;
            DPRINTF(ITTage, "table %d folded history before pred:%d\n",
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
    assert(threadHistory[tid].gHist ==
        &threadHistory[tid].globalHistory[threadHistory[tid].ptGhist]);
}

//if flush，recover global history
void
ITTAGE_BLK::squash_recover(ThreadID tid, bool taken,
                        ITTageBranchInfo *bi, bool isCondMisp,
                        unsigned numBrBefore)
{
    if (!speculativeHistUpdate) {
        /* If there are no speculative updates, no actions are needed */
        return;
    }

    ThreadHistory& tHist = threadHistory[tid];
    bool taken_recover[MaxNumBr];
    unsigned num_recover_br;
    // DPRINTF(Tage, "Restoring branch info: %lx; taken? %d; PathHistory:%x, "
    //         "pointer:%d\n", bi->branchPC, taken, bi->pathHist, bi->ptGhist);
    tHist.pathHist = bi->pathHist;  //recover pathHist
    tHist.ptGhist = bi->ptGhist;    //recover ptGhist
    tHist.gHist = &(tHist.globalHistory[tHist.ptGhist]);
    if (isCondMisp){
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
            DPRINTF(ITTage, "ghr squash folded index before recover: %d\n",
                    bi->ci[i]);
            DPRINTF(ITTage, "ghr squash recover cond number: %d\n",
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
ITTAGE_BLK::extraAltCalc(ITTageBranchInfo* bi)
{
    // do nothing. This is only used in some derived classes
    return;
}

void
ITTAGE_BLK::updateStats(bool* taken, bool* isCond, ITTageBranchInfo* bi)
{
    // if (isCond[0]){
    //     stats.tage_bank_0_has_update++;
    //     if (bi->provider[0] != TAGE_LONGEST_MATCH){
    //         stats.tage_bank_0_use_alt_pred++;
    //         if (bi->tagePred[0] == taken[0]){
    //             stats.tage_bank_0_alt_correct++;
    //         }else{
    //             stats.tage_bank_0_alt_wrong++;
    //         }
    //     }else{
    //         stats.tage_bank_0_use_provider_pred++;
    //         if (bi->tagePred[0] == taken[0]){
    //             stats.tage_bank_0_use_provider_correct++;
    //         }else{
    //             stats.tage_bank_0_use_provider_wrong++;
    //         }
    //     }

    //     if (bi->tagePred[0] != taken[0]){
    //         stats.tage_bank_0_mispred++;
    //     }
    //     if (bi->hitBank[0] > 0 && bi->provider[0] != TAGE_LONGEST_MATCH){
    //         stats.tage_bank_0_na++;
    //     }
    //     DPRINTF(Tage, "yyw test slot[0] update pc\n");
    //     if (bi->hitBank[0] > 0){
    //         stats.tage_bank_0_use_provider_hit++;
    //         DPRINTF(Tage, "slot0 hit tage table:%d\n", bi->hitBank[0]);
    //     }else{
    //         DPRINTF(Tage, "slot0 miss tage table, tage pred:%d\n",
    //                 bi->tagePred[0]);
    //     }
    // }

    // if (isCond[1]){
    //     stats.tage_bank_1_has_update++;
    //     if (bi->provider[1] != TAGE_LONGEST_MATCH){
    //         stats.tage_bank_1_use_alt_pred++;
    //         if (bi->tagePred[1] == taken[1]){
    //             stats.tage_bank_1_alt_correct++;
    //         }else{
    //             stats.tage_bank_1_alt_wrong++;
    //         }
    //     }else{
    //         stats.tage_bank_1_use_provider_pred++;
    //         if (bi->tagePred[1] == taken[1]){
    //             stats.tage_bank_1_use_provider_correct++;
    //         }else{
    //             stats.tage_bank_1_use_provider_wrong++;
    //         }
    //     }

    //     if (bi->tagePred[1] != taken[1]){
    //         stats.tage_bank_1_mispred++;
    //     }
    //     if (bi->hitBank[1] > 0 && bi->provider[1] != TAGE_LONGEST_MATCH){
    //         stats.tage_bank_1_na++;
    //     }
    //     DPRINTF(Tage, "yyw test slot[1] update pc\n");
    //     if (bi->hitBank[1] > 0){
    //         stats.tage_bank_1_use_provider_hit++;
    //         DPRINTF(Tage, "slot1 hit tage table:%d\n", bi->hitBank[1]);
    //     }else{
    //         DPRINTF(Tage, "slot1 miss tage table, tage pred:%d\n",
    //                         bi->tagePred[1]);
    //     }
    // }
}

unsigned
ITTAGE_BLK::getGHR(ThreadID tid, ITTageBranchInfo *bi) const
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

ITTAGE_BLK::ITTAGEBLKStats::ITTAGEBLKStats(
    statistics::Group *parent, unsigned nHistoryTables)
    : statistics::Group(parent),
      ADD_STAT(ittage_mispred,
                statistics::units::Count::get(),
                        "ittage_mispred"),
      ADD_STAT(ittage_reset_u,
                statistics::units::Count::get(),
                        "ittage_reset_u"),
      ADD_STAT(ittage_has_update,
                statistics::units::Count::get(),
                        "ittage_has_update"),
      ADD_STAT(ittage_use_provider_hit,
                statistics::units::Count::get(),
                        "ittage_use_provider_hit"),
      ADD_STAT(ittage_allocate_success,
                statistics::units::Count::get(),
                        "ittage_allocate_success"),
      ADD_STAT(ittage_allocate_failure,
                statistics::units::Count::get(),
                        "ittage_allocate_failure"),
      ADD_STAT(ittage_use_provider_pred,
                statistics::units::Count::get(),
                        "ittage_use_provider_pred"),
      ADD_STAT(ittage_use_provider_correct,
                statistics::units::Count::get(),
                        "ittage_use_provider_correct"),
      ADD_STAT(ittage_use_provider_wrong,
                statistics::units::Count::get(),
                        "ittage_use_provider_wrong"),
      ADD_STAT(ittage_use_alt_pred,
                statistics::units::Count::get(),
                        "ittage_use_alt_pred"),
      ADD_STAT(ittage_alt_correct,
                statistics::units::Count::get(),
                        "ittage_alt_correct"),
      ADD_STAT(ittage_alt_wrong,
                statistics::units::Count::get(),
                        "ittage_alt_wrong"),
      ADD_STAT(ittage_alt_differs,
                statistics::units::Count::get(),
                        "ittage_alt_differs"),
      ADD_STAT(ittage_use_alt_on_na_ctr_updated,
                statistics::units::Count::get(),
                        "ittage_use_alt_on_na_ctr_updated"),
      ADD_STAT(ittage_use_alt_on_na_ctr_inc,
                statistics::units::Count::get(),
                        "ittage_use_alt_on_na_ctr_inc"),
      ADD_STAT(ittage_use_alt_on_na_ctr_dec,
                statistics::units::Count::get(),
                        "ittage_use_alt_on_na_ctr_dec"),
      ADD_STAT(ittage_na,
                statistics::units::Count::get(),
                        "ittage_na"),
      ADD_STAT(ittage_use_na_correct,
                statistics::units::Count::get(),
                        "ittage_use_na_correct"),
      ADD_STAT(ittage_use_na_wrong,
                statistics::units::Count::get(),
                        "ittage_use_na_wrong")
{

}

unsigned
ITTAGE_BLK::getTageCtrBits() const
{
    return tagTableCounterBits;
}

int
ITTAGE_BLK::getPathHist(ThreadID tid) const
{
    return threadHistory[tid].pathHist;
}

bool
ITTAGE_BLK::isSpeculativeUpdateEnabled() const
{
    return speculativeHistUpdate;
}

ITTAGE_BLK::ITTageBranchInfo*
ITTAGE_BLK::makeBranchInfo() {
    return new ITTageBranchInfo(*this);
}

void
ITTAGE_BLK::free_mem(ITTAGE_BLK::ITTageBranchInfo * &bi){
    /** free memory*/
    delete bi;
}

} // namespace branch_prediction
} // namespace gem5

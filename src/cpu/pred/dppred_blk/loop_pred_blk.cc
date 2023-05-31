#include "cpu/pred/dppred_blk/loop_pred_blk.hh"

#include <queue>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "debug/LoopPred.hh"
#include "params/LOOP_PRED_BLK.hh"

namespace gem5
{

namespace branch_prediction
{

LOOP_PRED_BLK::LOOP_PRED_BLK(const LOOP_PRED_BLKParams &p)
   : SimObject(p), logSizeLoopPred(p.logSizeLoopPred),
    loopTableAgeBits(p.loopTableAgeBits),
    loopTableConfidenceBits(p.loopTableConfidenceBits),
    loopTableTagBits(p.loopTableTagBits),
    loopTableIterBits(p.loopTableIterBits),
    logLoopTableAssoc(p.logLoopTableAssoc),
    confidenceThreshold((1 << p.loopTableConfidenceBits) - 1),
    loopTagMask((1 << p.loopTableTagBits) - 1),
    loopNumIterMask((1 << p.loopTableIterBits) - 1),
    loopSetMask((1 << (p.logSizeLoopPred - p.logLoopTableAssoc)) - 1),
    loopUseCounter(-1),
    withLoopBits(p.withLoopBits),
    useDirectionBit(p.useDirectionBit),
    useSpeculation(p.useSpeculation),
    useHashing(p.useHashing),
    restrictAllocation(p.restrictAllocation),
    initialLoopIter(p.initialLoopIter),
    initialLoopAge(p.initialLoopAge),
    optionalAgeReset(p.optionalAgeReset),
    stats(this)
{
    assert(initialLoopAge <= ((1 << loopTableAgeBits) - 1));
    ltable = new LoopPredEntry[1ULL << logSizeLoopPred];
}


LOOP_PRED_BLK::BranchInfo*
LOOP_PRED_BLK::makeBranchInfo(unsigned logSizeLoopPred)
{
    return new BranchInfo(logSizeLoopPred);
}

int
LOOP_PRED_BLK::lindex(Addr pc_in, unsigned instShiftAmt) const
{
    // The loop table is implemented as a linear table
    // If associativity is N (N being 1 << logLoopTableAssoc),
    // the first N entries are for set 0, the next N entries are for set 1,
    // and so on.
    // Thus, this function calculates the set and then it gets left shifted
    // by logLoopTableAssoc in order to return the index of the first of the
    // N entries of the set
    Addr pc = pc_in >> instShiftAmt;
    if (useHashing) {
        pc ^= pc_in;
    }
    return ((pc & loopSetMask) << logLoopTableAssoc);
}

int
LOOP_PRED_BLK::finallindex(int index, int lowPcBits, int way) const
{
    return (useHashing ? (index ^ ((lowPcBits >> way) << logLoopTableAssoc)) :
                         (index))
           + way;
}

//loop prediction: only used if high confidence
bool
LOOP_PRED_BLK::getLoop(Addr pc, BranchInfo* bi, bool speculative,
                       unsigned instShiftAmt,
                       unsigned br, unsigned ftqIdx) const
{
    bi->loopHit[br] = -1;
    bi->loopPredValid[br] = false;
    bi->loopIndex[br] = lindex(pc, instShiftAmt);

    if (useHashing) {
        unsigned pcShift = logSizeLoopPred - logLoopTableAssoc;
        bi->loopIndexB[br] = (pc >> pcShift) & loopSetMask;
        bi->loopTag[br] = (pc >> pcShift) ^
                            (pc >> (pcShift + loopTableTagBits));
        bi->loopTag[br] &= loopTagMask;
    } else {
        unsigned pcShift = instShiftAmt + logSizeLoopPred - logLoopTableAssoc;
        bi->loopTag[br] = (pc >> pcShift) & loopTagMask;
        // bi->loopIndexB is not used without hash
    }

    for (int i = 0; i < (1 << logLoopTableAssoc); i++) {
        int idx = finallindex(bi->loopIndex[br], bi->loopIndexB[br], i);
        if (ltable[idx].tag == bi->loopTag[br]) {
            bi->loopHit[br] = i;
            bi->loopPredValid[br] = speculative ? calcConf(idx) &&
                                                ltable[idx].useIterSpec
                                                : calcConf(idx);
            bi->currentIter[br] = ltable[idx].currentIter;
            bi->currentIterSpec[br] = ltable[idx].currentIterSpec;

            uint16_t iter = speculative ? ltable[idx].currentIterSpec
                                        : ltable[idx].currentIter;

            if (speculative){
                DPRINTF(LoopPred,
                    "loop predict for branch pc: %lx, br: %d, "
                     "currentIterSpec: %d, currentIter: %d, "
                     "loopPredValid: %d, ftqIdx: %d\n",
                    pc, br, bi->currentIterSpec[br],
                    bi->currentIter[br], bi->loopPredValid[br], ftqIdx);
            }
            if (iter + 1 == ltable[idx].numIter) {
                return useDirectionBit ? !(ltable[idx].dir) : false;
            } else {
                return useDirectionBit ? (ltable[idx].dir) : true;
            }
        }
    }
    return false;
}

bool
LOOP_PRED_BLK::calcConf(int index) const
{
    return ltable[index].confidence >= confidenceThreshold;
}

void
LOOP_PRED_BLK::specLoopUpdate(bool taken, BranchInfo* bi,
                              unsigned br, bool isSquash)
{
    bool is_loop_end;
    if (bi->loopHit[br]>=0) {
        int index = finallindex(bi->loopIndex[br],
                                bi->loopIndexB[br], bi->loopHit[br]);
        is_loop_end = useDirectionBit ? taken != ltable[index].dir :
                                        taken != true;
        if (is_loop_end) {
            ltable[index].currentIterSpec = 0;
            if (!isSquash){
                DPRINTF(LoopPred,
                    "currentIterSpec reset, index: %d\n",
                    index);
            }
        } else {
            ltable[index].currentIterSpec =
                (ltable[index].currentIterSpec + 1) & loopNumIterMask;
            if (!isSquash){
                DPRINTF(LoopPred,
                    "currentIterSpec added, index: %d\n",
                    index);
            }
        }
        if (isSquash){
            DPRINTF(LoopPred,
                "Loop end squash after update currentIterSpec: %d\n",
                ltable[index].currentIterSpec);
            if (is_loop_end){
                ltable[index].useIterSpec = true;
                DPRINTF(LoopPred,
                    "Loop end squash set useIterSpec, index: %d\n",
                    index);
            }else{
                ltable[index].useIterSpec = false;
                DPRINTF(LoopPred,
                    "Loop end squash reset useIterSpec, index: %d\n",
                    index);
            }
        }
    }
}

bool
LOOP_PRED_BLK::optionalAgeInc() const
{
    return false;
}

void
LOOP_PRED_BLK::loopUpdate(Addr pc, bool taken, BranchInfo* bi,
                          bool tage_pred, unsigned br, unsigned ftqIdx)
{
    int idx = finallindex(bi->loopIndex[br],
                        bi->loopIndexB[br], bi->loopHit[br]);
    if (bi->loopHit[br] >= 0) {
        DPRINTF(LoopPred,
                "Loop End commit pc: %lx, br: %d, "
                "numIter: %d, currentIter: %d, ftqIdx: %d, taken: %d\n",
                pc, br, ltable[idx].numIter, ltable[idx].currentIter,
                ftqIdx, taken);
        //already a hit
        if (bi->loopPredValid[br]) {
            if (taken != bi->loopPred[br]) {
                // free the entry
                DPRINTF(LoopPred,
                    "age decrease because of pred wrong pc: %lx, br: %d, "
                    "numIter: %d, currentIter: %d, taken: %d\n",
                    pc, br, ltable[idx].numIter,
                    ltable[idx].currentIter, taken);
                ltable[idx].numIter = 0;
                //ltable[idx].tag = 0;
                ltable[idx].age = 0;
                ltable[idx].confidence = 0;
                //ltable[idx].currentIter = 0;
                //ltable[idx].currentIterSpec = 0;
                ltable[idx].useIterSpec = false;
                // unsignedCtrUpdate(ltable[idx].age, false,
                //                              loopTableAgeBits);
                //return;
            } else if (bi->loopPred[br] != tage_pred || optionalAgeInc()) {
                DPRINTF(LoopPred,
                    "age add because of pred right pc: %lx, br: %d, "
                    "numIter: %d, currentIter: %d, taken: %d\n", pc, br,
                    ltable[idx].numIter,
                    ltable[idx].currentIter, taken);
                unsignedCtrUpdate(ltable[idx].age, true,
                                            loopTableAgeBits);
            }
        }

        ltable[idx].currentIter =
            (ltable[idx].currentIter + 1) & loopNumIterMask;
        if (ltable[idx].currentIter > ltable[idx].numIter) {
            ltable[idx].confidence = 0;
            if (ltable[idx].numIter != 0) {
                // free the entry
                DPRINTF(LoopPred,
                    "currentIter greater than numIter, free entry pc: %lx, "
                    "br: %d, numIter: %d, currentIter: %d, taken: %d\n",
                    pc, br, ltable[idx].numIter,
                    ltable[idx].currentIter, taken);
                ltable[idx].confidence = 0;
                ltable[idx].numIter = 0;
                //ltable[idx].tag = 0;
                ltable[idx].useIterSpec = false;
                if (optionalAgeReset) {
                    ltable[idx].age = 0;
                }
            }
        }

        if (taken != (useDirectionBit ? ltable[idx].dir : true)) {
            // if hit and not taken, and committed loop number matches,
            // loop is Authenticated one time.
            if (ltable[idx].currentIter == ltable[idx].numIter) {
                DPRINTF(LoopPred,
                "Loop End detected successfully, add conf pc: %lx, br: %d, "
                "numIter: %d\n", pc, br, ltable[idx].numIter);
                //just do not predict when the loop count is 1 or 2
                if (ltable[idx].numIter < 3) {
                    // free the entry
                    DPRINTF(LoopPred,
                        "Loop number less than 3, free entry pc: %lx, "
                        "br: %d, currentIter: %d, numIter: %d\n",
                        pc, br, ltable[idx].currentIter,
                        ltable[idx].numIter);
                    ltable[idx].numIter = 0;
                    //ltable[idx].tag = 0;
                    ltable[idx].useIterSpec = false;
                    if (optionalAgeReset) {
                        ltable[idx].age = 0;
                    }
                    ltable[idx].confidence = 0;
                }else{
                    //add confidence
                    unsignedCtrUpdate(ltable[idx].confidence, true,
                                  loopTableConfidenceBits);
                }
            } else {
                //if numIter=0, this entry is the taken cond br
                //allocated last time;
                //now it is detected as loop Preliminarily
                if (ltable[idx].numIter == 0) {
                    // first complete nest;
                    ltable[idx].confidence = 0;
                    ltable[idx].numIter = ltable[idx].currentIter;
                    DPRINTF(LoopPred,
                    "Loop End numIter allocate pc: %lx, br: %d, numIter: %d\n",
                    pc, br, ltable[idx].numIter);
                } else {
                    //not the same number of iterations as last time: free the
                    //entry
                    DPRINTF(LoopPred,
                        "Loop End detected incorrectly, free entry pc: %lx, "
                        "br: %d, currentIter: %d, numIter: %d\n",
                        pc, br, ltable[idx].currentIter,
                        ltable[idx].numIter);
                    ltable[idx].numIter = 0;
                    //ltable[idx].tag = 0;
                    ltable[idx].useIterSpec = false;
                    if (optionalAgeReset) {
                        ltable[idx].age = 0;
                    }
                    ltable[idx].confidence = 0;
                }
            }
            ltable[idx].currentIter = 0;
            //ltable[idx].currentIterSpec[bank] = 0;
        }else{
            if (ltable[idx].currentIter == ltable[idx].numIter) {
                //free entry
                DPRINTF(LoopPred,
                "currentIter equals to numIter, but taken, free entry pc: "
                "%lx, br: %d, numIter: %d, currentIter: %d, taken: %d\n",
                    pc, br, ltable[idx].numIter,
                    ltable[idx].currentIter, taken);
                ltable[idx].confidence = 0;
                ltable[idx].numIter = 0;
                //ltable[idx].tag = 0;
                ltable[idx].useIterSpec = false;
                if (optionalAgeReset) {
                    ltable[idx].age = 0;
                }
            }
        }

    } else if (useDirectionBit ? (bi->predTaken[br] != taken) : taken) {
        // restrictAllocation will constraint frequency of
        // allocating taken cond bt to loop table
        if ((rand() & 3) == 0 || !restrictAllocation) {
            //try to allocate an entry on taken branch
            //randomly find a way(age==0) to allocate
            int nrand = rand();
            for (int i = 0; i < (1 << logLoopTableAssoc); i++) {
                int loop_hit = (nrand + i) & ((1 << logLoopTableAssoc) - 1);
                idx = finallindex(bi->loopIndex[br],
                                    bi->loopIndexB[br], loop_hit);
                if (ltable[idx].age == 0) {
                    DPRINTF(LoopPred,
                    "Allocating loop pred entry for branch pc: %lx, br: %d\n",
                    pc, br);
                    //ignored if no useDirectionBit
                    ltable[idx].dir = !taken;
                    ltable[idx].tag = bi->loopTag[br];
                    ltable[idx].numIter = 0;
                    ltable[idx].age = initialLoopAge;
                    ltable[idx].confidence = 0;
                    ltable[idx].currentIter = initialLoopIter;
                    ltable[idx].currentIterSpec = initialLoopIter;
                    ltable[idx].useIterSpec = false;
                    break;
                } else {
                    unsignedCtrUpdate(ltable[idx].age, false,
                                             loopTableAgeBits);
                }
                if (restrictAllocation) {
                    break;
                }
            }
        }
    }
}

void
LOOP_PRED_BLK::loopPredict(ThreadID tid, Addr startAddr,
                            bool* cond_branch, int8_t* offset,
                            bool* pred_taken, BranchInfo* &bi,
                            unsigned instShiftAmt, unsigned ftqIdx,
                            bool *confidence)
{
    Addr branch_pc[MaxNumBr];
    for (unsigned br = 0; br < MaxNumBr; ++br){
        branch_pc[br] = startAddr + offset[br]*2;
    }

    bi = new BranchInfo(logSizeLoopPred);

    for (unsigned j = 0; j < (1 << logSizeLoopPred); ++j){
        bi->ltable[j].currentIterSpec = ltable[j].currentIterSpec;
        bi->ltable[j].useIterSpec = ltable[j].useIterSpec;
    }
    // DPRINTF(LoopPred,
    //             "index0 currentIterSpec: %d\n",
    //             ltable[0].currentIterSpec);

    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (cond_branch[br]) {
            // loop prediction
            bi->loopPred[br] = getLoop(branch_pc[br], bi, useSpeculation,
                                            instShiftAmt, br, ftqIdx);

            if (bi->loopPredValid[br]) {
                pred_taken[br] = bi->loopPred[br];
                bi->loopPredUsed[br] = true;
                //confidence[br] = true;
            }
            if (pred_taken[br]){
                break;
            }
        }
    }
}

void
LOOP_PRED_BLK::specUpdateHist(ThreadID tid, Addr startAddr,
                            bool* cond_branch, int8_t* offset,
                            bool* pred_taken, BranchInfo* bi)
{
    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (cond_branch[br]) {
            specLoopUpdate(pred_taken[br], bi, br, false);
            if (pred_taken[br]){
                break;
            }
        }
    }
}

void
LOOP_PRED_BLK::squash(Addr startAddr, int8_t offset,
                      ThreadID tid, bool taken,
                      BranchInfo *bi, bool isCondMisp,
                      unsigned numBrBefore, unsigned ftqIdx)
{
    bool taken_recover[MaxNumBr];
    unsigned num_recover_br;
    Addr branch_pc = startAddr + offset*2;
    DPRINTF(LoopPred,
            "squash happens pc: %lx, ftqIdx: %d\n", branch_pc, ftqIdx);

    for (unsigned j = 0; j < (1 << logSizeLoopPred); ++j){
        ltable[j].currentIterSpec = bi->ltable[j].currentIterSpec;
        ltable[j].useIterSpec = bi->ltable[j].useIterSpec;
    }
    if (isCondMisp){
        if (numBrBefore == 0){
            taken_recover[0] = taken;
            taken_recover[1] = false;
            num_recover_br = 1;
            if (bi->loopPredUsed[0]){
                DPRINTF(LoopPred,
                    "loop end squash pc: %lx, br: %d, dir: %d ",
                    "currentIterSpec: %d, currentIter: %d\n",
                    branch_pc, 0, taken,
                    bi->currentIterSpec[0], bi->currentIter[0]);
            }
        }else if (numBrBefore == 1){
            taken_recover[0] = false;
            taken_recover[1] = taken;
            num_recover_br = 2;
            if (bi->loopPredUsed[1]){
                DPRINTF(LoopPred,
                    "loop end squash pc: %lx, br: %d, dir: %d ",
                    "currentIterSpec: %d, currentIter: %d\n",
                    branch_pc, 1, taken,
                    bi->currentIterSpec[1], bi->currentIter[1]);
            }
        }else{
            assert(false);
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
    }

    //update
    for (unsigned br = 0; br < num_recover_br; ++br){
        specLoopUpdate(taken_recover[br], bi, br, true);
    }
}

void
LOOP_PRED_BLK::updateStats(bool taken, bool prev_pred_taken,
                            BranchInfo* bi, unsigned br)
{
    if (bi->loopPredUsed[br]){
        if (bi->loopPred[br] != prev_pred_taken){
            if (taken == bi->loopPred[br]) {
                stats.loop_right_sc_wrong++;
            } else {
                stats.loop_wrong_sc_right++;
            }
        }else{
            if (taken == bi->loopPred[br]) {
                stats.loop_right_sc_right++;
            } else {
                stats.loop_wrong_sc_wrong++;
            }
        }
    }
}

void
LOOP_PRED_BLK::condBranchUpdate(ThreadID tid, Addr startAddr, unsigned ftqIdx,
                                bool* cond_branch, int8_t* offset,
                                bool* taken, bool* tage_pred,
                                BranchInfo* bi, unsigned instShiftAmt)
{
    Addr branch_pc[MaxNumBr];
    for (unsigned br = 0; br < MaxNumBr; ++br){
        branch_pc[br] = startAddr + offset[br]*2;
        // DPRINTF(LoopPred,
        //             "startAddr is: %lx, pc: %x, br: %d,
        //             isCond: %d\n", startAddr, branch_pc[br], br,
        //             cond_branch[br]);
    }

    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (cond_branch[br]){
            updateStats(taken[br], tage_pred[br], bi, br);
            if (bi->loopPredUsed[br] &&
                bi->loopPred[br] != tage_pred[br]){
                int idx = finallindex(bi->loopIndex[br],
                                bi->loopIndexB[br], bi->loopHit[br]);
                if (taken[br] != bi->loopPred[br]) {
                    DPRINTF(LoopPred,
                    "tage_sc right, loop pred wrong, Loop End pc: %lx, "
                    "br: %d, numIter: %d, currentIter: %d\n",
                        branch_pc[br], br, ltable[idx].numIter,
                        ltable[idx].currentIter);
                }
            }
            bi->loopPred[br] = getLoop(branch_pc[br], bi, false,
                                   instShiftAmt, br, 0);
            loopUpdate(branch_pc[br], taken[br], bi,
                        tage_pred[br], br, ftqIdx);
        }
    }

}

void
LOOP_PRED_BLK::free_mem(BranchInfo * &bi){
    /** free memory*/
    delete bi;
}

LOOP_PRED_BLK::LOOP_PRED_BLKStats::LOOP_PRED_BLKStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(loop_right_sc_wrong, statistics::units::Count::get(),
               "Number of times the loop_right_sc_wrong"),
      ADD_STAT(loop_wrong_sc_right, statistics::units::Count::get(),
               "Number of times the loop_wrong_sc_right"),
      ADD_STAT(loop_right_sc_right, statistics::units::Count::get(),
               "Number of times the loop_right_sc_right"),
      ADD_STAT(loop_wrong_sc_wrong, statistics::units::Count::get(),
               "Number of times the loop_wrong_sc_wrong")
{
}

} // namespace branch_prediction
} // namespace gem5

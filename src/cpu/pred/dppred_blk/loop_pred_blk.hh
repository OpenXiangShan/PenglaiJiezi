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

#ifndef __CPU_PRED_LOOP_PRED_BLK_HH__
#define __CPU_PRED_LOOP_PRED_BLK_HH__

#include <vector>
#include "base/statistics.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/o3/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "cpu/static_inst.hh"
#include "debug/LoopPred.hh"
#include "params/LOOP_PRED_BLK.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace branch_prediction
{
class LOOP_PRED_BLK : public SimObject
{
  public:
    LOOP_PRED_BLK(const LOOP_PRED_BLKParams &p);

  protected:
    const unsigned logSizeLoopPred;
    const unsigned loopTableAgeBits;
    const unsigned loopTableConfidenceBits;
    const unsigned loopTableTagBits;
    const unsigned loopTableIterBits;
    const unsigned logLoopTableAssoc;
    const uint8_t confidenceThreshold;
    const uint16_t loopTagMask;
    const uint16_t loopNumIterMask;
    const int loopSetMask;
    int8_t loopUseCounter;
    unsigned withLoopBits;

    const bool useDirectionBit;
    const bool useSpeculation;
    const bool useHashing;
    const bool restrictAllocation;
    const unsigned initialLoopIter;
    const unsigned initialLoopAge;
    const bool optionalAgeReset;

    struct LOOP_PRED_BLKStats : public statistics::Group
    {
        LOOP_PRED_BLKStats(statistics::Group *parent);
        statistics::Scalar loop_right_sc_wrong;
        statistics::Scalar loop_wrong_sc_right;
        statistics::Scalar loop_right_sc_right;
        statistics::Scalar loop_wrong_sc_wrong;
    } stats;

    // Tage Entry
    struct LoopPredEntry
    {
        //total loop number
        uint16_t numIter;
        //current iter number(commit)
        uint16_t currentIter;
        //current iter number(speculative)
        uint16_t currentIterSpec;
        uint8_t confidence;
        uint16_t tag;
        uint8_t age;
        bool useIterSpec;
        bool dir; // only for useDirectionBit

        LoopPredEntry(){
            numIter = 0;
            currentIter = 0;
            currentIterSpec = 0;
            confidence = 0;
            tag = 0;
            age = 0;
            dir = false;
            useIterSpec = false;
        }
    };

    LoopPredEntry *ltable;

    /**
     * Updates an unsigned counter based on up/down parameter
     * @param ctr Reference to counter to update.
     * @param up Boolean indicating if the counter is incremented/decremented
     * If true it is incremented, if false it is decremented
     * @param nbits Counter width.
     */
    static inline void unsignedCtrUpdate(uint8_t &ctr, bool up, unsigned nbits)
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
    static inline void signedCtrUpdate(int8_t &ctr, bool up, unsigned nbits)
    {
        if (up) {
            if (ctr < ((1 << (nbits - 1)) - 1))
                ctr++;
        } else {
            if (ctr > -(1 << (nbits - 1)))
                ctr--;
        }
    }

  public:
    // Primary branch history entry
    struct BranchInfo
    {
        uint16_t loopTag[MaxNumBr];
        uint16_t currentIter[MaxNumBr];
        uint16_t currentIterSpec[MaxNumBr];
        //loop pred result
        bool loopPred[MaxNumBr];
        //loop pred result greater than threshold
        bool loopPredValid[MaxNumBr];
        //if use loop pred result
        bool loopPredUsed[MaxNumBr];
        int  loopIndex[MaxNumBr];
        int  loopIndexB[MaxNumBr]; // only for useHashing
        int loopHit[MaxNumBr];
        bool predTaken[MaxNumBr];
        LoopPredEntry *ltable;

        BranchInfo(unsigned logSizeLoopPred) {
            for (int i = 0; i < MaxNumBr; i++){
                loopTag[i] = 0;
                loopIndex[i] = 0;
                loopIndexB[i] = 0;
                currentIter[i] = 0;
                loopPred[i] = false;
                loopPredValid[i] = false;
                loopPredUsed[i] = false;
                loopHit[i] = -1;
                predTaken[i] = false;
            }
            ltable = new LoopPredEntry[1ULL << logSizeLoopPred];
        }
        ~BranchInfo() {
            delete[] ltable;
        }
    };

    /**
     * Computes the index used to access the
     * loop predictor.
     * @param pc_in The unshifted branch PC.
     * @param instShiftAmt Shift the pc by as many bits
     */
    int lindex(Addr pc_in, unsigned instShiftAmt) const;

    /**
     * Computes the index used to access the
     * ltable structures.
     * It may take hashing into account
     * @param index Result of lindex function
     * @param lowPcBits PC bits masked with set size
     * @param way Way to be used
     */
    int finallindex(int lindex, int lowPcBits, int way) const;

    /**
     * Get a branch prediction from the loop
     * predictor.
     * @param pc The unshifted branch PC.
     * @param bi Pointer to information on the
     * prediction.
     * @param speculative Use speculative number of iterations
     * @param instShiftAmt Shift the pc by as many bits (if hashing is not
     * used)
     * @result the result of the prediction, if it could be predicted
     */
    bool getLoop(Addr pc, BranchInfo* bi, bool speculative,
                       unsigned instShiftAmt,
                       unsigned br, unsigned ftqIdx) const;

   /**
    * Updates the loop predictor.
    * @param pc The unshifted branch PC.
    * @param taken The actual branch outcome.
    * @param bi Pointer to information on the
    * prediction recorded at prediction time.
    * @param tage_pred tage prediction of the branch
    */
    void loopUpdate(Addr pc, bool taken, BranchInfo* bi,
                          bool tage_pred, unsigned br, unsigned ftqIdx);

    /**
     * Speculatively updates the loop predictor
     * iteration count (only for useSpeculation).
     * @param taken The predicted branch outcome.
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    void specLoopUpdate(bool taken, BranchInfo* bi,
                              unsigned br, bool isSquash);

    /**
     * Update LTAGE for conditional branches.
     * @param branch_pc The unshifted branch PC.
     * @param taken Actual branch outcome.
     * @param tage_pred Prediction from TAGE
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     * @param instShiftAmt Number of bits to shift instructions
     */
    void condBranchUpdate(ThreadID tid, Addr startAddr, unsigned ftqIdx,
                        bool* cond_branch, int8_t* offset,
                        bool* taken, bool* tage_pred,
                        BranchInfo* bi, unsigned instShiftAmt);

    /**
     * Get the loop prediction
     * @param tid The thread ID to select the global
     * histories to use.
     * @param branch_pc The unshifted branch PC.
     * @param cond_branch True if the branch is conditional.
     * @param bi Reference to wrapping pointer to allow storing
     * derived class prediction information in the base class.
     * @param prev_pred_taken Result of the TAGE prediction
     * @param instShiftAmt Shift the pc by as many bits
     * @param instShiftAmt Shift the pc by as many bits
     * @result the prediction, true if taken
     */
    void loopPredict(ThreadID tid, Addr startAddr,
                            bool* cond_branch, int8_t* offset,
                            bool* pred_taken, BranchInfo* &bi,
                            unsigned instShiftAmt, unsigned ftqIdx,
                            bool *confidence);

    /**
     * Update the stats
     * @param taken Actual branch outcome
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    void updateStats(bool taken, bool prev_pred_taken,
                            BranchInfo* bi, unsigned br);

    void specUpdateHist(ThreadID tid, Addr startAddr,
                            bool* cond_branch, int8_t* offset,
                            bool* pred_taken, BranchInfo* bi);

    void squash(Addr startAddr, int8_t offset,
                      ThreadID tid, bool taken,
                      BranchInfo *bi, bool isCondMisp,
                      unsigned numBrBefore, unsigned ftqIdx);

    virtual bool calcConf(int index) const;

    virtual bool optionalAgeInc() const;

    virtual BranchInfo *makeBranchInfo(unsigned logSizeLoopPred);

    /**
     * Gets the value of the loop use counter
     * @return the loop use counter value
     */
    int8_t getLoopUseCounter() const
    {
        return loopUseCounter;
    }

    size_t getSizeInBits() const;

    void free_mem(BranchInfo * &bi);
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_LOOP_PRED_BLK_HH__

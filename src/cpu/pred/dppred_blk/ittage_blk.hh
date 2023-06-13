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

#ifndef __CPU_PRED_ITTAGE_BLK_HH__
#define __CPU_PRED_ITTAGE_BLK_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/o3/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "cpu/static_inst.hh"
#include "debug/ITTage.hh"
#include "params/ITTAGE_BLK.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class ITTAGE_BLK : public SimObject
{
  public:
    ITTAGE_BLK(const ITTAGE_BLKParams &p);
    ~ITTAGE_BLK();

  protected:
    // Prediction Structures
    // Tage Entry
    struct ITTageEntry
    {
        bool valid;
        int8_t ctr;  //2bit
        uint16_t tag;
        uint8_t u;
        Addr target;
        ITTageEntry() : valid(false), ctr(-2), tag(0), u(0), target(0){}
    };


    // Folded History Table - compressed history
    // to mix with instruction PC to index partially
    // tagged tables.
    struct FoldedHistory
    {
        unsigned comp;
        int compLength;
        int origLength;
        int outpoint;
        int bufferSize;
        uint8_t *folded_hist_vec;

        FoldedHistory()
        {
            comp = 0;
        }

        // original_length：global history length；
        // compressed_length：history length after folding
        void init(int original_length, int compressed_length)
        {
            origLength = original_length;  //e.g.：32
            compLength = compressed_length; //e.g.：11
            outpoint = original_length % compressed_length;  //e.g.：10
            folded_hist_vec = new uint8_t[compLength];
        }

        void free()
        {
            delete[] folded_hist_vec;
        }

        /**update foldeded history*/
        void update(uint8_t * h, unsigned updateNum)
        {
            if (origLength <= compLength){
                if (updateNum == 1){
                    /** h[0]：newest dir bit, h[origLength]:oldest bit*/
                    comp = (comp << 1) | h[0];
                    comp &= (1ULL << origLength) - 1;
                }else if (updateNum == 2) {
                    /** h[0]/h[1]：newest/second newest dir bit,
                    * h[origLength+1]/h[origLength]: oldest/second oldest bit*/
                    comp = (comp << 2) | (h[1] << 1) | h[0];
                    comp &= (1ULL << origLength) - 1;
                }else if (updateNum == 0){
                    return;
                }else{
                    assert(false);
                }
            }else{
                if (updateNum == 1){
                    /** h[0]：newest dir bit, h[origLength]:oldest dir bit*/
                    comp = (comp << 1) | h[0];
                    comp ^= h[origLength] << outpoint;
                    comp ^= (comp >> compLength);
                    comp &= (1ULL << compLength) - 1;
                }else if (updateNum == 2) {
                    /** h[0]/h[1]：newest/second newest dir bit,
                    * h[origLength+1]/h[origLength]: oldest/second oldest bit*/
                    comp = (comp << 2) | (h[1] << 1) | h[0];
                    comp ^= (comp >> compLength);
                    comp ^= h[origLength] << outpoint;
                    comp ^= h[origLength + 1] << ((outpoint+1)%compLength);
                    comp &= (1ULL << compLength) - 1;
                }else if (updateNum == 0){
                    return;
                }else{
                    assert(false);
                }
            }
        }

        void check_folded_history(uint8_t * global_hist){
            unsigned folded_hist = 0;

            int iter_num = origLength/compLength;
            for (int i =0; i<compLength; i++){
                folded_hist_vec[i] = 0;
                for (int j =0; j<iter_num; j++){
                    folded_hist_vec[i] = folded_hist_vec[i] ^
                                        global_hist[i+j*compLength];
                }
                if (i < outpoint){
                    folded_hist_vec[i] = folded_hist_vec[i] ^
                                    global_hist[i+iter_num*compLength];
                }
                folded_hist = folded_hist + (folded_hist_vec[i] << i);
            }
            assert(folded_hist == comp);
        }
    };

  public:
    // provider type
    enum
    {
        BIMODAL_ONLY = 0,
        TAGE_LONGEST_MATCH,
        BIMODAL_ALT_MATCH,
        TAGE_ALT_MATCH,
        LAST_TAGE_PROVIDER_TYPE = TAGE_ALT_MATCH
    };

    // Primary branch history entry
    struct ITTageBranchInfo
    {
        int pathHist;
        int ptGhist;
        uint8_t ghr121[121];

        int hitBank;   //provider table
        int8_t hitBankCtr; //provider ctr
        Addr hitBankTarget; //provider target
        int hitBankIndex;  //provider idx

        int altBank;   //alt table
        int8_t altBankCtr; //alt ctr
        Addr altBankTarget; //alt target
        int altBankIndex;  //alt idx

        Addr branchPC;
        Addr ittagePredTarget;
        Addr finalPredTarget;

        bool pseudoNewAlloc;
        //TAGE_LONGEST_MATCH/TAGE_ALT_MATCH/BIMODAL_ALT_MATCH
        unsigned provider;
        //is altpred/base table confidence dir diff with final tage result
        bool altDiffers;
        bool taken; //confidence dir

        // Pointer to dynamically allocated storage
        // to save table indices and folded histories.
        // To do one call to new instead of five.
        int *storage;
        // Pointers to actual saved array within the dynamically
        // allocated storage.
        int *tableIndices; //record every tage table's idx
        int *tableTags; //record every tage table's tag
        int *ci; //record every tage table's folded history for idx
        int *ct0; //record every tage table's folded history for tag0
        int *ct1; //record every tage table's folded history for tag1

        ITTageBranchInfo(const ITTAGE_BLK &ittage)
        {
            pathHist = 0;
            ptGhist = 0;
            for (int i = 0; i < 121; i++) {
                ghr121[i] = 0;
            }

            hitBank = 0;
            hitBankCtr = 0;
            hitBankTarget = 0;
            hitBankIndex = 0;  //provider idx

            altBank = 0;   //alt table
            altBankCtr = 0; //alt ctr
            altBankTarget = 0;
            altBankIndex = 0;  //alt idx

            ittagePredTarget = 0; //ittage result
            finalPredTarget = 0;
            pseudoNewAlloc = false; //lack provider
            provider = BIMODAL_ONLY;

            int sz = ittage.nHistoryTables + 1;
            storage = new int [sz * 5];
            tableIndices = storage;
            tableTags = storage + sz;
            ci = tableTags + sz;
            ct0 = ci + sz;
            ct1 = ct0 + sz;
        }

        virtual ~ITTageBranchInfo()
        {
            delete storage;
        }
    };

    virtual ITTageBranchInfo *makeBranchInfo();

    void free_mem(ITTageBranchInfo * &bi);

    /**
     * Computes the index used to access the
     * bimodal table.
     * @param pc_in The unshifted branch PC.
     */
    virtual int bindex(Addr pc_in) const;

    /**
     * Computes the index used to access a
     * partially tagged table.
     * @param tid The thread ID used to select the
     * global histories to use.
     * @param pc The unshifted branch PC.
     * @param bank The partially tagged table to access.
     */
    virtual int gindex(ThreadID tid, Addr pc, int bank) const;

    /**
     * Utility function to shuffle the path history
     * depending on which tagged table we are accessing.
     * @param phist The path history.
     * @param size Number of path history bits to use.
     * @param bank The partially tagged table to access.
     */
    virtual int F(int phist, int size, int bank) const;

    /**
     * Computes the partial tag of a tagged table.
     * @param tid the thread ID used to select the
     * global histories to use.
     * @param pc The unshifted branch PC.
     * @param bank The partially tagged table to access.
     */
    virtual uint16_t gtag(ThreadID tid, Addr pc, int bank) const;

    /**
     * Updates a direction counter based on the actual
     * branch outcome.
     * @param ctr Reference to counter to update.
     * @param taken Actual branch outcome.
     * @param nbits Counter width.
     */
    template<typename T>
    static void ctrUpdate(T & ctr, bool taken, int nbits);

    /**
     * Updates an unsigned counter based on up/down parameter
     * @param ctr Reference to counter to update.
     * @param up Boolean indicating if the counter is incremented/decremented
     * If true it is incremented, if false it is decremented
     * @param nbits Counter width.
     */
    static void unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits);


   /**
    * (Speculatively) updates the global branch history.
    * @param h Reference to pointer to global branch history.
    * @param dir (Predicted) outcome to update the histories
    * with.
    * @param tab
    * @param PT Reference to path history.
    */
    void updateGHist(uint8_t * &h, bool* dir, unsigned condNum,
                        uint8_t * tab, int &PT);

    /**
     * Update TAGE. Called at execute to repair histories on a misprediction
     * and at commit to update the tables.
     * @param tid The thread ID to select the global
     * histories to use.
     * @param branch_pc The unshifted branch PC.
     * @param taken Actual branch outcome.
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    void update(ThreadID tid, Addr branch_pc,
                Addr target, bool isIndirect,
                ITTageBranchInfo* bi, bool &press);

   /**
    * (Speculatively) updates global histories (path and direction).
    * Also recomputes compressed (folded) histories based on the
    * branch direction.
    * @param tid The thread ID to select the histories
    * to update.
    * @param branch_pc The unshifted branch PC.
    * @param taken (Predicted) branch direction.
    * @param b Wrapping pointer to TageBranchInfo (to allow
    * storing derived class prediction information in the
    * base class).
    */
    virtual void updateHistories(
        ThreadID tid, Addr branch_pc, bool* taken, bool* isCond,
        ITTageBranchInfo* b,  bool speculative,
        Addr target = MaxAddr);

    /**
     * Restores speculatively updated path and direction histories.
     * Also recomputes compressed (folded) histories based on the
     * correct branch outcome.
     * This version of squash() is called once on a branch misprediction.
     * @param tid The Thread ID to select the histories to rollback.
     * @param taken The correct branch outcome.
     * @param bp_history Wrapping pointer to TageBranchInfo (to allow
     * storing derived class prediction information in the
     * base class).
     * @param target The correct branch target
     * @post bp_history points to valid memory.
     */
    virtual void squash_recover(
        ThreadID tid, bool taken, ITTageBranchInfo *bi, bool isCondMisp,
        unsigned numBrBefore);

    /**
     * TAGE prediction called from TAGE::predict
     * @param tid The thread ID to select the global
     * histories to use.
     * @param branch_pc The unshifted branch PC.
     * @param cond_branch True if the branch is conditional.
     * @param bi Pointer to the TageBranchInfo
     */
    Addr predict(ThreadID tid, Addr branch_pc,
                bool* pred_taken, bool* pred_isCond,
                ITTageBranchInfo* &bi, Addr ftb_target,
                bool *confidence);

    /**
     * Update the stats
     * @param taken Actual branch outcome
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    virtual void updateStats(bool* taken, bool* isCond, ITTageBranchInfo* bi);

    /**
     * Instantiates the TAGE table entries
     */
    virtual void buildTageTables();

    /**
     * On a prediction, calculates the TAGE indices and tags for
     * all the different history lengths
     */
    virtual void calculateIndicesAndTags(
        ThreadID tid, Addr branch_pc, ITTageBranchInfo* bi);

    /**
     * Handles Allocation and U bits reset on an update
     */
    virtual void handleAllocAndUReset(
        bool alloc, Addr target, ITTageBranchInfo* bi);

    /**
     * Handles the U bits reset
     */
    virtual void handleUReset();

    /**
     * Handles the update of the TAGE entries
     */
    virtual void handleTAGEUpdate(
        Addr branch_pc, Addr target, ITTageBranchInfo* bi, bool &press);

    /**
     * Algorithm for resetting a single U counter
     */
    virtual void resetUctr(uint8_t & u);

    /**
     * Extra steps for calculating altTaken
     * For this base TAGE class it does nothing
     */
    virtual void extraAltCalc(ITTageBranchInfo* bi);

    virtual bool isHighConfidence(ITTageBranchInfo* bi) const
    {
        return false;
    }

    unsigned getGHR(ThreadID tid, ITTageBranchInfo *bi) const;
    unsigned getTageCtrBits() const;
    int getPathHist(ThreadID tid) const;
    bool isSpeculativeUpdateEnabled() const;

  protected:
    const unsigned nHistoryTables;
    const unsigned tagTableCounterBits;
    const unsigned tagTableUBits;
    //global history buffer size， =2097152
    const unsigned histBufferSize;
    const unsigned minHist;
    const unsigned maxHist;
    std::vector<unsigned> histLengths;
    const unsigned pathHistBits;

    std::vector<unsigned> tagTableTagWidths;
    std::vector<int> logTagTableSizes;   //log of every table size
    ITTageEntry **gtable;

    // Keep per-thread histories to
    // support SMT.
    struct ThreadHistory
    {
        // Speculative path history
        // (LSB of branch address)
        int pathHist;

        // Speculative branch direction history (circular buffer)
        // pointer to the head of global history buffer
        uint8_t *globalHistory;

        // Pointer to most recent branch outcome
        // in global history buffer after update
        uint8_t* gHist;

        // Index to most recent branch outcome
        // in global history buffer after update
        int ptGhist;

        // Speculative folded histories.
        // pointer to foldedHist，to compute idx
        FoldedHistory *computeIndices;
        // pointer to foldedHist，to compute tag
        FoldedHistory *computeTags[2];
    };

    // speculative history for all threads
    std::vector<ThreadHistory> threadHistory;

    /**
     * Initialization of the folded histories
     */
    virtual void initFoldedHistories(ThreadHistory & history);

    int *tableIndices; //pointer to idxs of all tables
    int *tableTags;    //pointer to tags of all tables

    int64_t tCounter;   //tage table update times
    int bankTickCtrs;
    uint64_t logUResetPeriod;   //log of period to reset u bit
    const int64_t initialTCounterValue;
    unsigned numUseAltOnNa;   //=128
    unsigned useAltOnNaBits;  //=4
    unsigned maxNumAlloc;   //=1

    // Tells which tables are active
    // (for the base TAGE implementation all are active)
    std::vector<bool> noSkip;
    const bool speculativeHistUpdate;
    const unsigned instShiftAmt;
    bool initialized;
    struct ITTAGEBLKStats : public statistics::Group
    {
        ITTAGEBLKStats(statistics::Group *parent, unsigned nHistoryTables);
        // stats
        statistics::Scalar ittage_mispred;
        statistics::Scalar ittage_reset_u;
        statistics::Scalar ittage_has_update;
        statistics::Scalar ittage_use_provider_hit;
        statistics::Scalar ittage_allocate_success;
        statistics::Scalar ittage_allocate_failure;
        statistics::Scalar ittage_use_provider_pred;
        statistics::Scalar ittage_use_provider_correct;
        statistics::Scalar ittage_use_provider_wrong;
        statistics::Scalar ittage_use_alt_pred;
        statistics::Scalar ittage_alt_correct;
        statistics::Scalar ittage_alt_wrong;
        statistics::Scalar ittage_alt_differs;
        statistics::Scalar ittage_use_alt_on_na_ctr_updated;
        statistics::Scalar ittage_use_alt_on_na_ctr_inc;
        statistics::Scalar ittage_use_alt_on_na_ctr_dec;
        statistics::Scalar ittage_na;
        statistics::Scalar ittage_use_na_correct;
        statistics::Scalar ittage_use_na_wrong;
    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_ITTAGE_BLK_HH__

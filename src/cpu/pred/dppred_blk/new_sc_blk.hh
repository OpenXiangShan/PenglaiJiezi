#ifndef __CPU_PRED_NEW_SC_BLK_HH__
#define __CPU_PRED_NEW_SC_BLK_HH__

#include <vector>

#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "params/New_SC_BLK.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace branch_prediction {
class New_SC_BLK : public SimObject
{
    public:
        New_SC_BLK(const New_SC_BLKParams &p);

    struct SCEntry
    {
        int8_t ctr[MaxNumBr];
        SCEntry(){
            for (unsigned i = 0; i < MaxNumBr; ++i){
                ctr[i] = 0;
            }
        }
    };

    template<typename T>
    inline void ctrUpdate(T & ctr, bool taken, int nbits) {
        assert(nbits <= sizeof(T) << 3);
        if (nbits > 0) {
            if (taken) {
                if (ctr < ((1 << (nbits - 1)) - 1))
                    ctr++;
            } else {
                if (ctr > -(1 << (nbits - 1)))
                    ctr--;
            }
        }
    }
    // histories used for the statistical corrector
    struct SCThreadHistory
    {
        SCThreadHistory() {
            bwHist = 0;
            numOrdinalHistories = 0;
            imliCount = 0;
            globalHist = 0;
        }

        ~SCThreadHistory()
        {
            delete[] localHistories;
        }
        int64_t bwHist;  // backward global history
        int64_t imliCount;  //loop counter
        int64_t globalHist; // global history
        //local history indexed ctr table
        std::vector<int64_t> * localHistories;
        std::vector<int> shifts;
        unsigned numOrdinalHistories;

        unsigned getEntry(Addr pc, unsigned idx) //calculate idx
        {
            return (pc ^ (pc >> shifts[idx])) & (localHistories[idx].size()-1);
        }

        void setNumOrdinalHistories(unsigned num)
        {
            numOrdinalHistories = num;
            assert(num > 0);
            shifts.resize(num);
            localHistories = new std::vector<int64_t> [num];
        }

        void initLocalHistory(int ordinal, int numHistories, int shift)
        {
            assert((ordinal >= 1) && (ordinal <= numOrdinalHistories));
            shifts[ordinal - 1] = shift;
            assert(isPowerOf2(numHistories));
            localHistories[ordinal - 1].resize(numHistories, 0);
        }

        int64_t getLocalHistory(int ordinal, Addr pc)
        {
            assert((ordinal >= 1) && (ordinal <= numOrdinalHistories));
            unsigned idx = ordinal - 1;
            return localHistories[idx][getEntry(pc, idx)];
        }

        void updateLocalHistory(
            int ordinal, Addr branch_pc, bool taken, Addr extraXor = 0)
        {
            assert((ordinal >= 1) && (ordinal <= numOrdinalHistories));
            unsigned idx = ordinal - 1;

            unsigned entry = getEntry(branch_pc, idx);
            int64_t hist =  (localHistories[idx][entry] << 1) + taken;
            if (extraXor) {
                hist = hist ^ extraXor;
            }
            localHistories[idx][entry] = hist;
        }
    };

    // For SC history we use global (i.e. not per thread) non speculative
    // histories, as some of the strucures needed are quite big and it is not
    // reasonable to make them per thread and it would be difficult to
    // rollback on miss-predictions
    SCThreadHistory * scHistory;

    unsigned logBias;

    //log size for threshold counters tables
    unsigned logSizeUp;
    unsigned logSizeUps;

    /** global backward branch history indexed tables */
    //number of global backward branch history indexed tables
    unsigned bwnb;
    //global backward branch history indexed table depth
    unsigned logBwnb;
    //global backward branch history length
    std::vector<int> bwm;
    //pointer to global backward branch history indexed tables
    std::vector<SCEntry> * bwgehl;
    //extra weight table
    std::vector<SCEntry> wbw;

    /** First local history indexed tables */
    //number of entries for first local histories
    unsigned numEntriesFirstLocalHistories;
    //number of first local history indexed tables
    unsigned lnb;
    //log number of first local history indexed tables
    unsigned logLnb;
    //local history lengths for all first local history indexed tables
    std::vector<int> lm;
    //pointer to first local history tables
    std::vector<SCEntry> * lgehl;
    //extra weight table
    std::vector<SCEntry> wl;

    /** Second local history indexed tables */
    unsigned numEntriesSecondLocalHistories;
    const unsigned snb;
    const unsigned logSnb;
    std::vector<int> sm;
    std::vector<SCEntry> * sgehl;
    std::vector<SCEntry> ws;

    /** Third local history indexed tables */
    unsigned numEntriesThirdLocalHistories;
    const unsigned tnb;
    const unsigned logTnb;
    std::vector<int> tm;
    std::vector<SCEntry> * tgehl;
    std::vector<SCEntry> wt;

    /** loop counter indexed tables */
    //number of loop counter indexed table
    unsigned inb;
    unsigned logInb;
    std::vector<int> im;
    std::vector<SCEntry> * igehl;
    std::vector<SCEntry> wi;

    /** global history indexed table */
    //number of global history indexed table
    unsigned gnb;
    unsigned logGnb;
    std::vector<int> gm;
    std::vector<SCEntry> * ggehl;
    std::vector<SCEntry> wg;

    /** bias tables */
    std::vector<SCEntry> bias;
    std::vector<SCEntry> biasSK;
    std::vector<SCEntry> biasBank;
    std::vector<SCEntry> wb; //extra weight for bias table

    //threshold counter
    int updateThreshold[MaxNumBr];
    //threshold counters tables
    std::vector<int> pUpdateThreshold[MaxNumBr];

    // The two counters used to choose between TAGE ang SC on High Conf
    // TAGE/Low Conf SC
    unsigned chooserConfWidth;
    unsigned updateThresholdWidth;
    unsigned pUpdateThresholdWidth;
    unsigned extraWeightsWidth;

    unsigned scCountersWidth;
    int lWeightInitValue;
    int bwWeightInitValue;
    int iWeightInitValue;
    int initialUpdateThresholdValue;
    bool hasThreeBias = true;

    struct New_SC_BLKStats : public statistics::Group
    {
        New_SC_BLKStats(statistics::Group *parent);
        statistics::Scalar sc_correct_tage_wrong;
        statistics::Scalar sc_wrong_tage_correct;
        statistics::Scalar sc_used;
        statistics::Scalar tage_used;
        statistics::Scalar num_update_cond_br;
        statistics::Scalar condCorrect;
        statistics::Scalar condWrong;
    } stats;

    struct BranchInfo : public SCThreadHistory
    {
        BranchInfo(unsigned numEntriesFirstLocalHistories,
                   unsigned numEntriesSecondLocalHistories,
                   unsigned numEntriesThirdLocalHistories)
        {
            for (unsigned br = 0; br < MaxNumBr; ++br){
                lowConf[br] = false;
                highConf[br] = false;
                altConf[br] = false;
                medConf[br] = false;
                scPred[br] = false;
                lsum[br] = 0;
                thres[br] = 0;
                predBeforeSC[br] = false;
                usedScPred[br] = false;
            }
            setNumOrdinalHistories(3); //3个local history
            //local history的深度
            initLocalHistory(1, numEntriesFirstLocalHistories, 2);
            initLocalHistory(2, numEntriesSecondLocalHistories, 5);
            initLocalHistory(3, numEntriesThirdLocalHistories, 3);
        }

        // confidences calculated on tage and used on the statistical
        // correction
        bool lowConf[MaxNumBr];
        bool highConf[MaxNumBr];
        bool altConf[MaxNumBr];
        bool medConf[MaxNumBr];

        bool scPred[MaxNumBr];
        int lsum[MaxNumBr];
        int thres[MaxNumBr];
        bool predBeforeSC[MaxNumBr];
        bool usedScPred[MaxNumBr];
    };

    BranchInfo *makeBranchInfo();
    SCThreadHistory *makeThreadHistory();

    void initBias();

    void scPredict(ThreadID tid, Addr branch_pc, bool* cond_branch,
                     bool* always_taken, BranchInfo* &bi,
                     bool* pred_taken, bool* bias_bit,
                     int8_t* conf_ctr, unsigned conf_bits,
                     int* hitBank, int* altBank,
                     Addr* corrTarget, bool *confidence);

    unsigned getIndBias(Addr branch_pc,
                                BranchInfo* bi, bool b, unsigned br) const;

    unsigned getIndBiasSK(Addr branch_pc, BranchInfo* bi, unsigned br) const;

    unsigned getIndBiasBank( Addr branch_pc, BranchInfo* bi,
        int hitBank, int altBank, unsigned br);

    unsigned getIndUpd(Addr branch_pc) const;
    unsigned getIndUpds(Addr branch_pc) const;

    void gPredictions(ThreadID tid, Addr branch_pc, BranchInfo* bi,
                            int & lsum, unsigned br,
                            unsigned bank, int& thres);

    int64_t gIndex(Addr branch_pc, int64_t bhist, int logs, int nbr, int i);

    int gIndexLogsSubstr(int nbr, int i);

    int gPredict(Addr branch_pc, int64_t hist,
        std::vector<int> & length, std::vector<SCEntry> * tab, int nbr,
        int logs, std::vector<SCEntry> & w, unsigned br, unsigned bank);

    void gUpdate(Addr branch_pc, bool taken, int64_t hist,
                   std::vector<int> & length, std::vector<SCEntry> * tab,
                   int nbr, int logs, std::vector<SCEntry> & w,
                   BranchInfo* bi, unsigned br, unsigned bank);

    void squash(ThreadID tid, bool taken, Addr branch_pc,
                        BranchInfo *bi, bool isCondMisp,
                        unsigned numBrBefore, Addr* corrTarget);

    void initGEHLTable(
        unsigned numLenghts, std::vector<int> lengths,
        std::vector<SCEntry> * & table, unsigned logNumEntries,
        std::vector<SCEntry> & w, int8_t wInitValue);

    void scHistoryUpdate(Addr branch_pc,
                        bool taken, Addr corrTarget);

    void specUpdateHist(Addr branch_pc, bool* cond_branch,
                        bool* pred_taken, Addr* corrTarget);

    void gUpdates(ThreadID tid, Addr pc, bool taken,
            BranchInfo* bi, unsigned br, unsigned bank);

    void updateStats(bool taken, BranchInfo *bi, unsigned br);
    void free_mem(BranchInfo * &bi);

    void condBranchUpdate(ThreadID tid, Addr branch_pc, bool* cond_branch,
                bool* taken, BranchInfo *bi, bool* bias_bit, int* hitBank,
                int* altBank);

};

}  // namespace branch_prediction
}  // namespace gem5
#endif  // __CPU_NEW_SC_BLK_HH__

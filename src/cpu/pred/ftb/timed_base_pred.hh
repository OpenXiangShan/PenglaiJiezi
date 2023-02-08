#ifndef __CPU_PRED_FTB_TIMED_BASE_PRED_HH__
#define __CPU_PRED_FTB_TIMED_BASE_PRED_HH__


#include <boost/dynamic_bitset.hpp>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "sim/sim_object.hh"
#include "params/TimedBaseFTBPredictor.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

class TimedBaseFTBPredictor: public SimObject
{
    public:

    typedef TimedBaseFTBPredictorParams Params;

    TimedBaseFTBPredictor(const Params &params);

    virtual void tickStart() {}
    virtual void tick() {}
    // make predictions, record in stage preds
    virtual void putPCHistory(Addr startAddr,
                              const boost::dynamic_bitset<> &history,
                              std::vector<FullFTBPrediction> &stagePreds) {}

    virtual std::shared_ptr<void> getPredictionMeta() { return nullptr; }

    virtual void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}
    virtual void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) {}
    virtual void update(const FetchStream &entry) {}
    virtual unsigned getDelay() {return 0;}

    int componentIdx;
    int getComponentIdx() { return componentIdx; }
    void setComponentIdx(int idx) { componentIdx = idx; }


    bool enableDB {false};
    void setDB(DataBase *db) {
        _db = db;
    }
    virtual void setTrace() {}
    DataBase *_db;
};

} // namespace ftb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_FTB_TIMED_BASE_PRED_HH__

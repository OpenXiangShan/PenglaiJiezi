#ifndef __CPU_PRED_RAS_LK_BLK_HH__
#define __CPU_PRED_RAS_LK_BLK_HH__

#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "debug/RAS.hh"
#include "params/RAS_BLK.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace branch_prediction {

class RAS_BLK : public SimObject
{
    public:

        RAS_BLK(const RAS_BLKParams &p);

        struct RASSpecEntry
        {
            bool valid;
            Addr retAddr;
            unsigned next_top_idx;
            RASSpecEntry() : valid(false), retAddr(0),
                            next_top_idx(0) {}
        };

        struct RASCmtEntry
        {
            Addr retAddr;
            RASCmtEntry() : retAddr(0) {}
        };

        struct Meta
        {
            unsigned ls_wr_idx;
            unsigned ls_top_idx;
            unsigned cs_spec_top_idx;
            Addr top_retAddr;
            std::vector<bool> stack_valid;
            Meta(unsigned numSpecEntries){
                stack_valid.resize(numSpecEntries);
            }
        };

        Addr getRetAddr(Meta *rasMeta);
        Meta *createMeta();

        Addr getRasTopAddr();

        void specUpdate(bool isCall, bool isRet, Addr pushAddr);

        void predict(bool taken_is_ret, Addr &target, bool taken_is_call,
                Addr endAddr, Meta* &rasmeta, bool *confidence);

        void squash_recover(Meta *recoverRasMeta, bool isCall,
                        bool isRet, Addr pushAddr);

        void commit(Addr pushAddr, bool isCall, bool isRet);

        void free_mem(Meta * &meta_ptr);

    private:

        void push(Addr retAddr);

        void pop();

        void ptrInc(unsigned &ptr, bool isSpecStack);

        void ptrDec(unsigned &ptr, bool isSpecStack);

        unsigned numSpecEntries;
        unsigned numCmtEntries;

        unsigned ls_wr_idx;
        unsigned ls_top_idx;
        unsigned cs_cmt_top_idx;
        unsigned cs_spec_top_idx;

        std::vector<RASSpecEntry> lk_stack;  //spec link stack
        std::vector<RASCmtEntry> cmt_stack;  //cmt stack
};


}  // namespace branch_prediction
}  // namespace gem5
#endif  // __CPU_PRED_RAS_LK_BLK_HH__

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

        struct RASEntry
        {
            Addr retAddr;
            unsigned next_top_idx;
            RASEntry() : retAddr(0), next_top_idx(0) {}
        };

        struct Meta
        {
            unsigned wr_idx;
            unsigned top_idx;
            RASEntry tos;
        };

        Addr getRetAddr(Meta *rasMeta);
        Meta *createMeta();

        Addr getRasTopAddr();

        void specUpdate(bool isCall, bool isRet, Addr pushAddr);

        void predict(bool taken_is_ret, Addr &target, bool taken_is_call,
                Addr endAddr, Meta* &rasmeta, bool *confidence);

        void squash_recover(Meta *recoverRasMeta, bool isCall,
                        bool isRet, Addr pushAddr);

        void free_mem(Meta * &meta_ptr);

    private:

        void push(Addr retAddr);

        void pop();

        void ptrInc(unsigned &ptr);

        void ptrDec(unsigned &ptr);

        void printStack(const char *when) {
            DPRINTF(RAS, "printStack when %s: \n", when);
            for (unsigned i = 0; i < numEntries; i++) {
                DPRINTFR(RAS, "entry [%d], retAddr %#lx",
                            i, stack[i].retAddr);
                if (top_idx == i) {
                    DPRINTFR(RAS, " <-- top_idx");
                }
                if (wr_idx == i) {
                    DPRINTFR(RAS, " <-- wr_idx");
                }
                DPRINTFR(RAS, "\n");
            }
        }

        unsigned numEntries;

        unsigned ctrWidth;

        int maxCtr;

        unsigned wr_idx;
        unsigned top_idx;

        std::vector<RASEntry> stack;
};


}  // namespace branch_prediction
}  // namespace gem5
#endif  // __CPU_PRED_RAS_LK_BLK_HH__

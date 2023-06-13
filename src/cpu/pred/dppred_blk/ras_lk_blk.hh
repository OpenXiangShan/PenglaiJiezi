/*
 * Copyright (c) 2023 Todo
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

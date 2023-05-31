/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#ifndef __CPU_PRED_UBTB_BLK_HH__
#define __CPU_PRED_UBTB_BLK_HH__

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "params/UBTB_BLK.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class UBTB_BLK : public SimObject
{
  public:

    UBTB_BLK(const UBTB_BLKParams &p);

    struct UBTBCTR
    {
        int8_t ctr[MaxNumBr];    //2bit
        UBTBCTR(){
            for (int i = 0; i < MaxNumBr; i++){
                ctr[i] = 0;
            }
        }
    };

    struct UBTBBranchInfo
    {
        bool ubtb_hit;
        BTBEntry ubtb_hit_entry;
    };

  public:
    /** Creates a BTB with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the BTB.
     *  @param tagBits Number of bits for each tag in the BTB.
     *  @param instShiftAmt Offset amount for instructions to ignore alignment.
     */
    UBTB_BLK(unsigned numBr, unsigned numEntries,
            unsigned ways, unsigned tagBits,
            unsigned instShiftAmt, unsigned numThreads);

    ~UBTB_BLK();

    template<typename T>
    static void ctrUpdate(T & ctr, bool taken, int nbits);

    /** Returns the tag bits of a given address.
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    unsigned getIndex(Addr instPC, ThreadID tid);
    Addr getTag(Addr instPC);

    void unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits);

    void reset();

    /** Looks up an address in the BTB. Must call valid() first on the address.
     *  @param inst_PC The address of the branch to look up.
     *  @param tid The thread id.
     *  @return Returns the target of the branch.
     */
    void lookup(Addr instPC, ThreadID tid,
                BTBEntry *&hit_btb_entry, UBTBCTR *&hit_ctr);

    bool predict(Addr &startAddr, Addr &endAddr,
                 Addr &target, unsigned &pred_offset,
                 ThreadID tid, UBTBBranchInfo* &bi,
                 bool &taken_is_indir, bool &taken_is_ret,
                 bool &taken_is_call, bool *isCond,
                 bool *pred_taken, bool *confidence);

    /** Checks if a branch is in the BTB.
     *  @param inst_PC The address of the branch to look up.
     *  @param tid The thread id.
     *  @return Whether or not the branch exists in the BTB.
     */
    bool valid(Addr instPC, ThreadID tid);

    /** Updates the BTB with the target of a branch.
     *  @param inst_pc The address of the branch being updated.
     *  @param target_pc The target address of the branch.
     *  @param tid The thread id.
     */
    bool alloc_update_lookup(Addr instPC, ThreadID tid,
                          BTBEntry &hit_btb_entry);

    BTBEntry allocate_merge(Addr start_addr,
              BTBEntry old_entry, BTBEntry new_entry);

    void allocate_btb_entry(Addr inst_pc, BTBEntry new_btb_entry,
                  ThreadID tid, bool false_hit);

    void update_btb_entry(Addr inst_pc, BTBEntry new_btb_entry,
                ThreadID tid, bool slot0_false_empty);

    void update_ctr(Addr inst_pc, ThreadID tid,
                     bool* taken, bool* isCond);

    bool btb_entry_cmp(BTBEntry btb_entry0, BTBEntry btb_entry1);

    void update(Addr startAddr, ThreadID tid, bool *actual_taken, bool* isCond,
                    BTBEntry new_btb_entry, UBTBBranchInfo * &bi,
                    bool allocate_btb, bool update_btb, bool false_hit,
                    bool slot0_false_empty);

    void free_mem(UBTBBranchInfo * &bi);

  private:
    /** Returns the index into the BTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the BTB.
     */

    /** The actual BTB. */
    BTBEntry **btb;
    uint8_t **lru;
    UBTBCTR **ctr;


    /** numBr = 2 */
    unsigned numBr;

    /** The number of entries in the BTB. */
    unsigned numEntries;

    /** The number of ways in the BTB. */
    unsigned ways;

    /** The index mask. */
    unsigned idxMask;

    /** The number of tag bits per entry. */
    unsigned tagBits;

    /** The tag mask. */
    unsigned tagMask;

    /** Number of bits to shift PC when calculating index. */
    unsigned instShiftAmt;

    /** Number of bits to shift PC when calculating tag. */
    unsigned tagShiftAmt;

    /** Log2 NumThreads used for hashing threadid */
    unsigned log2NumThreads;

    /** ctr bits of ubtb entry */
    unsigned ctrWidth = 2;

    unsigned predBlkSize;

    unsigned lruBits = 3;

  protected:
    struct UBTBBLKStats : public statistics::Group
    {
        UBTBBLKStats(statistics::Group *parent);
        // stats
        statistics::Scalar ubtb_false_hit;
        statistics::Scalar ubtb_ctr_update;
    } stats;

};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_UBTB_HH__

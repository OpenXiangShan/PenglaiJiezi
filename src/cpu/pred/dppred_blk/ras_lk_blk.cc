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

#include "cpu/pred/dppred_blk/ras_lk_blk.hh"
#include "debug/RAS.hh"

namespace gem5 {
namespace branch_prediction {

RAS_BLK::RAS_BLK(const RAS_BLKParams &p)
    : SimObject(p),
    numSpecEntries(p.numSpecEntries),
    numCmtEntries(p.numCmtEntries)
{
    ls_wr_idx = 0;
    ls_top_idx = 0;
    cs_cmt_top_idx = numCmtEntries-1;
    cs_spec_top_idx = numCmtEntries-1;
    lk_stack.resize(numSpecEntries);
    cmt_stack.resize(numCmtEntries);
    for (auto &entry : lk_stack) {
        entry.valid = false;
        entry.next_top_idx = 0;
        entry.retAddr = 0x80000000L;
    }
    for (auto &entry : cmt_stack) {
        entry.retAddr = 0x80000000L;
    }
}

Addr
RAS_BLK::getRetAddr(Meta *rasMeta){
    return rasMeta->top_retAddr;
}

RAS_BLK::Meta *RAS_BLK::createMeta()
{
    Meta* meta_ptr = new Meta(numSpecEntries);
    meta_ptr->ls_top_idx = ls_top_idx;
    meta_ptr->ls_wr_idx = ls_wr_idx;
    meta_ptr->cs_spec_top_idx = cs_spec_top_idx;
    if (lk_stack[ls_top_idx].valid){
        meta_ptr->top_retAddr = lk_stack[ls_top_idx].retAddr;
    }else{
        meta_ptr->top_retAddr = cmt_stack[cs_spec_top_idx].retAddr;
    }
    for (unsigned i = 0; i < numSpecEntries; ++i){
        meta_ptr->stack_valid[i] = lk_stack[i].valid;
    }
    return meta_ptr;
}


Addr
RAS_BLK::getRasTopAddr()
{
    if (lk_stack[ls_top_idx].valid){
        return lk_stack[ls_top_idx].retAddr;
    }else{
        return cmt_stack[cs_spec_top_idx].retAddr;
    }
}

void
RAS_BLK::specUpdate(bool isCall, bool isRet, Addr pushAddr)
{
    // do push & pops on prediction
    if (isRet) {
        // do pop
        pop();
    }
    if (isCall) {
        push(pushAddr);
    }
}

void
RAS_BLK::predict(bool taken_is_ret, Addr &target, bool taken_is_call,
                Addr endAddr, Meta* &rasmeta, bool *confidence){

    rasmeta = createMeta();

    if (taken_is_ret){
        target = getRasTopAddr();
        confidence[1] = true;
        DPRINTF(RAS, "ras pred ret\n");
        specUpdate(false, true, 0);
    }

    if (taken_is_call){
        DPRINTF(RAS, "ras pred call\n");
        specUpdate(true, false, endAddr);
    }
}

void
RAS_BLK::squash_recover(Meta *recoverRasMeta, bool isCall,
                    bool isRet, Addr pushAddr)
{
    // recover sp and tos first
    ls_top_idx = recoverRasMeta->ls_top_idx;
    ls_wr_idx = recoverRasMeta->ls_wr_idx;
    cs_spec_top_idx = recoverRasMeta->cs_spec_top_idx;
    for (unsigned i = 0; i < numSpecEntries; ++i){
        lk_stack[i].valid = recoverRasMeta->stack_valid[i];
    }

    // do push & pops on control squash
    if (isRet) {
        DPRINTF(RAS, "ras recover update ret\n");
        DPRINTF(RAS, "pop addr is %x\n", getRasTopAddr());
        pop();
    }
    if (isCall) {
        DPRINTF(RAS, "ras recover update call\n");
        DPRINTF(RAS, "push addr is %x\n", pushAddr);
        push(pushAddr);
    }
}

void
RAS_BLK::commit(Addr pushAddr, bool isCall, bool isRet)
{
    if (isCall){
        ptrInc(cs_cmt_top_idx, false);
        cmt_stack[cs_cmt_top_idx].retAddr = pushAddr;
        DPRINTF(RAS, "call commit\n");
    }else if (isRet){
        ptrDec(cs_cmt_top_idx, false);
        DPRINTF(RAS, "ret commit\n");
    }
}


void
RAS_BLK::push(Addr retAddr)
{
    lk_stack[ls_wr_idx].retAddr = retAddr;
    lk_stack[ls_wr_idx].next_top_idx = ls_top_idx;
    lk_stack[ls_wr_idx].valid = true;
    ls_top_idx = ls_wr_idx;
    ptrInc(ls_wr_idx, true);
    ptrInc(cs_spec_top_idx, false);
}

void
RAS_BLK::pop()
{
    auto& tos = lk_stack[ls_top_idx];
    if (tos.valid){
        tos.valid = false;
        ls_top_idx = tos.next_top_idx;
    }
    ptrDec(cs_spec_top_idx, false);
}

void
RAS_BLK::ptrInc(unsigned &ptr, bool isSpecStack)
{
    if (isSpecStack){
        ptr = (ptr + 1) % numSpecEntries;
    }else{
        ptr = (ptr + 1) % numCmtEntries;
    }
}

void
RAS_BLK::ptrDec(unsigned &ptr, bool isSpecStack)
{
    if (ptr > 0) {
        ptr--;
    } else {
        if (isSpecStack){
            ptr = numSpecEntries - 1;
        }else{
            ptr = numCmtEntries - 1;
        }
    }
}

void
RAS_BLK::free_mem(RAS_BLK::Meta * &meta_ptr)
{
    delete meta_ptr;
}

}  // namespace branch_prediction
}  // namespace gem5

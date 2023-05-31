#include "cpu/pred/dppred_blk/ras_lk_blk.hh"

#include "debug/RAS.hh"

namespace gem5 {
namespace branch_prediction {

RAS_BLK::RAS_BLK(const RAS_BLKParams &p)
    : SimObject(p),
    numEntries(p.numEntries),
    ctrWidth(p.ctrWidth)
{
    wr_idx = 0;
    top_idx = 0;
    stack.resize(numEntries);
    maxCtr = (1 << ctrWidth) - 1;
    for (auto &entry : stack) {
        entry.next_top_idx = 0;
        entry.retAddr = 0x80000000L;
    }
}

Addr
RAS_BLK::getRetAddr(Meta *rasMeta){
    return rasMeta->tos.retAddr;
}

RAS_BLK::Meta *RAS_BLK::createMeta()
{
    Meta* meta_ptr = new Meta;
    meta_ptr->top_idx = top_idx;
    meta_ptr->wr_idx = wr_idx;
    meta_ptr->tos = stack[top_idx];
    return meta_ptr;
}


Addr
RAS_BLK::getRasTopAddr()
{
    return stack[top_idx].retAddr;
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
    printStack("after specUpdate");
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
    printStack("before recover");
    // recover sp and tos first
    top_idx = recoverRasMeta->top_idx;
    wr_idx = recoverRasMeta->wr_idx;

    printStack("after recover");

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
    printStack("after recover update");
}


void
RAS_BLK::push(Addr retAddr)
{
    // push new entry
    stack[wr_idx].retAddr = retAddr;
    stack[wr_idx].next_top_idx = top_idx;
    top_idx = wr_idx;
    ptrInc(wr_idx);
}

void
RAS_BLK::pop()
{
    auto& tos = stack[top_idx];
    top_idx = tos.next_top_idx;
}

void
RAS_BLK::ptrInc(unsigned &ptr)
{
    ptr = (ptr + 1) % numEntries;
}

void
RAS_BLK::ptrDec(unsigned &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numEntries - 1;
    }
}

void
RAS_BLK::free_mem(RAS_BLK::Meta * &meta_ptr)
{
    delete meta_ptr;
}

}  // namespace branch_prediction
}  // namespace gem5

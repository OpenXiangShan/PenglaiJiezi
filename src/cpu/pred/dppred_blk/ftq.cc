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

/* @file
 * Implementation of a TAGE branch predictor
 */

#include "cpu/pred/dppred_blk/ftq.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "cpu/o3/limits.hh"
#include "debug/Fetch.hh"
#include "debug/Ftq.hh"
#include "debug/Tage.hh"

namespace gem5
{
namespace branch_prediction
{
FTQ::FTQ(const FTQParams &params):
    SimObject(params),
    ftqSize(params.ftqSize)
{
    fetch_target_queue = new FtqEntry[ftqSize];
    pred_s1_idx = 0;
    pred_s1_ptr = fetch_target_queue;
    pred_s2_idx = 0;
    pred_s2_ptr = fetch_target_queue;
    fetch_idx = 0;
    fetch_ptr = fetch_target_queue;
    prefetch_idx = 0;
    prefetch_ptr = fetch_target_queue;
    commit_idx = 0;
    commit_ptr = fetch_target_queue;
}

FTQ::~FTQ(){
    delete []fetch_target_queue;
}


unsigned FTQ::ftq_idx_ptr_add(unsigned ftq_idx,
                              unsigned ftqSize,
                              FtqEntry * &ftq_ptr){
    if (ftq_idx == ftqSize-1 || ftq_idx == 2*ftqSize-1){
        ftq_ptr = fetch_target_queue;
    }else{
        ftq_ptr++;
    }

    if (ftq_idx == ftqSize*2-1){
        return 0;
    }else{
        return ++ftq_idx;
    }
}

unsigned FTQ::next_idx(unsigned idx){
    if (idx == ftqSize*2-1){
        return 0;
    }else{
        return ++idx;
    }
}

unsigned FTQ::previous_idx(unsigned idx){
    if (idx == 0){
        return (ftqSize*2-1);
    }else{
        return --idx;
    }
}

unsigned FTQ::get_curr_pred_s1_idx(){
   return pred_s1_idx;
}

unsigned FTQ::get_curr_pred_s2_idx(){
   return pred_s2_idx;
}

unsigned FTQ::get_curr_pred_s3_idx(){
   return pred_s3_idx;
}

unsigned FTQ::get_curr_fetch_idx(){
   return fetch_idx;
}

unsigned FTQ::get_curr_prefetch_idx(){
   return prefetch_idx;
}

unsigned FTQ::get_curr_commit_idx(){
   return commit_idx;
}

FTQ::FtqEntry FTQ::get_ftq_entry(unsigned &ftq_idx){
    return fetch_target_queue[ftq_idx%ftqSize];
}

bool FTQ::is_ftq_fetch_empty(){
    return (pred_s1_idx == fetch_idx);
    //return (pred_s3_idx == fetch_idx);
}

bool FTQ::is_ftq_prefetch_empty(){
    return (pred_s1_idx == prefetch_idx);
    //return (pred_s3_idx == prefetch_idx);
}

bool FTQ::is_ftq_commit_empty(){
    return (commit_idx == fetch_idx);
}

bool FTQ::is_ftq_full(){
    //unsigned logFtqSize = floorLog2(ftqSize);
    if (pred_s1_idx/ftqSize == commit_idx/ftqSize){
        assert(pred_s1_idx%ftqSize >= commit_idx%ftqSize);
    }else{
        assert(pred_s1_idx%ftqSize <= commit_idx%ftqSize);
    }
    return (pred_s1_idx - commit_idx == ftqSize ||
            pred_s1_idx - commit_idx == -ftqSize);
}

void FTQ::write_pdinfo_to_ftq(unsigned &ftq_idx, PdInfo pd_info){
    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    ftq_entry.pd_info = pd_info;
}

void FTQ::write_cfiindex_to_ftq(unsigned &ftq_idx, CfiInfo cfiIndex){
    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    ftq_entry.cfiIndex = cfiIndex;
}

void FTQ::write_target_to_ftq(unsigned &ftq_idx, Addr target){
    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    ftq_entry.target = target;
}

void FTQ::write_mispred_to_ftq(unsigned &ftq_idx,
                                    bool mispred, Addr pc){
    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    unsigned offset = (pc - ftq_entry.startAddr)/2;
    ftq_entry.mispred[offset] = mispred;
}

void FTQ::write_commitstatus_to_ftq(unsigned &ftq_idx,
                                    unsigned commit_status,
                                    unsigned offset){

    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    ftq_entry.commit_status[offset] = commit_status;
}

void FTQ::write_hitstatus_to_ftq(unsigned &ftq_idx,
                                 unsigned hit_status){

    FtqEntry &ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    ftq_entry.hit_status = hit_status;
}

void FTQ::push_back_to_ftq(FtqEntry &ftq_entry){
    ftq_entry.ftq_idx = pred_s1_idx;
    *pred_s1_ptr = ftq_entry;
    pred_s1_idx = ftq_idx_ptr_add(pred_s1_idx, ftqSize, pred_s1_ptr);
}

void FTQ::s2_update_to_ftq(FtqEntry &ftq_entry){
    assert(ftq_entry.ftq_idx == pred_s2_idx);
    *pred_s2_ptr = ftq_entry;
    pred_s2_idx = ftq_idx_ptr_add(pred_s2_idx, ftqSize, pred_s2_ptr);
}

void FTQ::s3_update_to_ftq(FtqEntry &ftq_entry){
    assert(ftq_entry.ftq_idx == pred_s3_idx);
    *pred_s3_ptr = ftq_entry;
    pred_s3_idx = ftq_idx_ptr_add(pred_s3_idx, ftqSize, pred_s3_ptr);
}

void FTQ::write_entry_to_ftq(unsigned ftq_idx, FtqEntry ftq_entry){
    FtqEntry &this_ftq_entry = fetch_target_queue[ftq_idx%ftqSize];
    this_ftq_entry = ftq_entry;

}

FTQ::FtqEntry FTQ::fetch_pop_from_ftq(){
    FtqEntry ftq_entry;
    ftq_entry = fetch_target_queue[fetch_idx%ftqSize];
    fetch_idx = ftq_idx_ptr_add(fetch_idx, ftqSize, fetch_ptr);
    return ftq_entry;
}

FTQ::FtqEntry FTQ::prefetch_pop_from_ftq(){
    FtqEntry ftq_entry;
    ftq_entry = fetch_target_queue[prefetch_idx%ftqSize];
    prefetch_idx = ftq_idx_ptr_add(prefetch_idx, ftqSize, prefetch_ptr);
    return ftq_entry;
}

bool FTQ::is_before_fetch_idx(unsigned ftq_idx){
    if (fetch_idx/ftqSize == ftq_idx/ftqSize){
        return (ftq_idx%ftqSize < fetch_idx%ftqSize);
    }else{
        return (ftq_idx%ftqSize > fetch_idx%ftqSize);
    }
}

bool FTQ::is_before_prefetch_idx(unsigned ftq_idx){
    if (prefetch_idx/ftqSize == ftq_idx/ftqSize){
        return (ftq_idx%ftqSize < prefetch_idx%ftqSize);
    }else{
        return (ftq_idx%ftqSize > prefetch_idx%ftqSize);
    }
}

void FTQ::recover_fetch_ptr(unsigned recover_idx){
    fetch_idx = recover_idx;
    fetch_ptr = &fetch_target_queue[recover_idx%ftqSize];
}

void FTQ::recover_prefetch_ptr(unsigned recover_idx){
    prefetch_idx = recover_idx;
    prefetch_ptr = &fetch_target_queue[recover_idx%ftqSize];
}

void FTQ::recover_pred_s1_ptr(unsigned recover_idx){
    pred_s1_idx = recover_idx;
    pred_s1_ptr = &fetch_target_queue[recover_idx%ftqSize];
}

void FTQ::recover_pred_s2_ptr(unsigned recover_idx){
    pred_s2_idx = recover_idx;
    pred_s2_ptr = &fetch_target_queue[recover_idx%ftqSize];
}

void FTQ::recover_pred_s3_ptr(unsigned recover_idx){
    pred_s3_idx = recover_idx;
    pred_s3_ptr = &fetch_target_queue[recover_idx%ftqSize];
}

std::vector<unsigned>
FTQ::get_intra_squashed_ftq_idxs(unsigned recover_idx){
    std::vector<unsigned> squashed_ftq_idxs;
    if (fetch_idx/ftqSize == recover_idx/ftqSize){
        assert(fetch_idx%ftqSize >= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize; i < fetch_idx%ftqSize; ++i){
            squashed_ftq_idxs.push_back(fetch_target_queue[i].ftq_idx);
        }
    }else{
        assert(fetch_idx%ftqSize <= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize; i < ftqSize; ++i){
            squashed_ftq_idxs.push_back(fetch_target_queue[i].ftq_idx);
        }
        for (unsigned i = 0; i < fetch_idx%ftqSize; ++i){
            squashed_ftq_idxs.push_back(fetch_target_queue[i].ftq_idx);
        }
    }
    return squashed_ftq_idxs;
}

std::vector<FTQ::FtqEntry>
FTQ::get_squash_s1_free_entries(unsigned recover_idx){
    std::vector<FTQ::FtqEntry> free_entries;
    if (pred_s1_idx/ftqSize == recover_idx/ftqSize){
        assert(pred_s1_idx%ftqSize >= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < pred_s1_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }else{
        assert(pred_s1_idx%ftqSize <= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
        for (unsigned i = 0; i < pred_s1_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }
    return free_entries;
}

std::vector<FTQ::FtqEntry>
FTQ::get_squash_s2_free_entries(unsigned recover_idx){
    std::vector<FTQ::FtqEntry> free_entries;
    if (pred_s2_idx/ftqSize == recover_idx/ftqSize){
        assert(pred_s2_idx%ftqSize >= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < pred_s2_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }else{
        assert(pred_s2_idx%ftqSize <= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
        for (unsigned i = 0; i < pred_s2_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }
    return free_entries;
}

std::vector<FTQ::FtqEntry>
FTQ::get_squash_s3_free_entries(unsigned recover_idx){
    std::vector<FTQ::FtqEntry> free_entries;
    if (pred_s3_idx/ftqSize == recover_idx/ftqSize){
        assert(pred_s3_idx%ftqSize >= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < pred_s3_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }else{
        assert(pred_s3_idx%ftqSize <= recover_idx%ftqSize);
        for (unsigned i = recover_idx%ftqSize+1; i < ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
        for (unsigned i = 0; i < pred_s3_idx%ftqSize; ++i){
            free_entries.push_back(fetch_target_queue[i]);
        }
    }
    return free_entries;
}


FTQ::FtqEntry FTQ::get_commit_idx_entry(){
    FtqEntry ftq_entry;
    ftq_entry = fetch_target_queue[commit_idx%ftqSize];
    return ftq_entry;
}

void FTQ::commit_retire_entry(){
    assert(!is_ftq_commit_empty());
    commit_idx = ftq_idx_ptr_add(commit_idx, ftqSize, commit_ptr);
}

bool FTQ::curr_entry_has_committed(){
    FtqEntry ftq_entry = fetch_target_queue[commit_idx%ftqSize];
    bool instr_has_committed;
    if (!is_ftq_commit_empty()){
        instr_has_committed = true;
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            if (ftq_entry.commit_status[i] == FtqCommitState::VALID ||
                ftq_entry.commit_status[i] == FtqCommitState::UNFETCHED){
                instr_has_committed = false;
                break;
            }
        }
    }else{
        instr_has_committed = false;
    }
    return instr_has_committed;
}

bool FTQ::next_entry_has_committed(){

    bool instr_has_committed;
    unsigned next_commit_idx;
    next_commit_idx = next_idx(commit_idx);
    if (!is_ftq_commit_empty() && next_commit_idx != fetch_idx){
        FtqEntry ftq_entry = fetch_target_queue[next_commit_idx%ftqSize];
        instr_has_committed = true;
        for (unsigned i = 0; i <= MaxInstrsPerBlock-1; ++i){
            if (ftq_entry.commit_status[i] == FtqCommitState::VALID ||
                ftq_entry.commit_status[i] == FtqCommitState::UNFETCHED){
                instr_has_committed = false;
                break;
            }
        }
    }else{
        instr_has_committed = false;
    }
    return instr_has_committed;
}

bool FTQ::curr_entry_should_up_bpu(){
    FtqEntry ftq_entry = fetch_target_queue[commit_idx%ftqSize];
    if (ftq_entry.hit_status == FtqHitStatus::H_HIT ||
        ftq_entry.cfiIndex.valid){
        return true;
    }else{
        return false;
    }
}

} // namespace branch_prediction
} // namespace gem5

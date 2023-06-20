#ifndef __CPU_PRED_FTQ_HH__
#define __CPU_PRED_FTQ_HH__

#include <vector>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/dppred_blk/comm.hh"
#include "cpu/pred/dppred_blk/comm_param.hh"
#include "cpu/pred/dppred_blk/tage_sc_i_blk.hh"
#include "cpu/timebuf.hh"
#include "params/FTQ.hh"

namespace gem5
{

namespace branch_prediction
{

class FTQ: public SimObject
{
  public:
    FTQ(const FTQParams &params);
    ~FTQ();

    //commitStateQueue
    enum FtqCommitState
    {
        UNFETCHED = 0,
        INVALID,
        VALID,
        COMMITED
    };

    //fetch_status
    enum FtqFetchStatus
    {
        F_TO_SEND = 0,
        F_SENT
    };

    //hit_status
    enum FtqHitStatus
    {
        H_NOT_HIT = 0,
        H_FALSE_HIT,
        H_HIT
    };

    struct CfiInfo
    {
      bool valid = false;
      unsigned cfiIndex = 0;  //0~15

    };

    struct FtqEntry
    {
      unsigned ftq_idx = 0;
      Addr startAddr = 0;
      Addr endAddr = 0;
      Addr target = 0;
      bool pred_taken = false;
      bool confidence = false;
      unsigned taken_offset;
      CfiInfo cfiIndex;  //0~15
      bool mispred[MaxInstrsPerBlock] = {0};
      unsigned commit_status[MaxInstrsPerBlock] = {0};
      unsigned fetch_status = {0};
      unsigned hit_status = {0};
      PdInfo pd_info;
      TAGE_SC_I_BLK::TageSCIBranchInfo tage_sc_i_bInfo;
      bool hasMispred(){
        for (int i = 0; i < MaxInstrsPerBlock; i++) {
          if (mispred[i]){
            return true;
          }
        }
        return false;
      }
    };

    FtqEntry get_ftq_entry(unsigned &ftq_idx);
    bool is_ftq_fetch_empty(); //no instr to fetch
    bool is_ftq_prefetch_empty(); //no instr to prefetch
    bool is_ftq_commit_empty(); //no instr to commit
    bool is_ftq_full();
    unsigned ftq_idx_ptr_add(unsigned ftq_idx,
                              unsigned ftqSize, FtqEntry * &ftq_ptr);
    unsigned next_idx(unsigned idx);
    unsigned previous_idx(unsigned idx);
    unsigned get_curr_pred_s1_idx();
    unsigned get_curr_pred_s2_idx();
    unsigned get_curr_pred_s3_idx();
    unsigned get_curr_fetch_idx();
    unsigned get_curr_prefetch_idx();
    unsigned get_curr_commit_idx();

    //s3 write
    //void write_predinfo_to_ftq(unsigned &ftq_idx, FtqEntry ftq_entry);

    //predecode write
    void write_pdinfo_to_ftq(unsigned &ftq_idx, PdInfo pd_info);

    //s1/s2/s3/predecode_squash/backend_squash write
    void write_cfiindex_to_ftq(unsigned &ftq_idx, CfiInfo cfiIndex);

    //s1/s2/s3/predecode_squash/backend_squash write
    void write_target_to_ftq(unsigned &ftq_idx, Addr target);

    //backend_squash write
    void write_mispred_to_ftq(unsigned &ftq_idx, bool mispred, Addr pc);

    //predecode/commit write
    void write_commitstatus_to_ftq(unsigned &ftq_idx,
                              unsigned commit_status, unsigned offset);

    //ifetch write
    //void write_fetchstatus_to_ftq(unsigned &ftq_idx, unsigned fetch_status);

    //s2/predecode_squash write
    void write_hitstatus_to_ftq(unsigned &ftq_idx, unsigned hit_status);

    void push_back_to_ftq(FtqEntry &ftq_entry);

    void s2_update_to_ftq(FtqEntry &ftq_entry);

    void s3_update_to_ftq(FtqEntry &ftq_entry);

    void write_entry_to_ftq(unsigned ftq_idx, FtqEntry ftq_entry);

    FtqEntry fetch_pop_from_ftq();

    FtqEntry prefetch_pop_from_ftq();

    bool is_before_fetch_idx(unsigned ftq_idx);
    bool is_before_prefetch_idx(unsigned ftq_idx);

    void recover_fetch_ptr(unsigned recover_idx);
    void recover_prefetch_ptr(unsigned recover_idx);
    void recover_pred_s1_ptr(unsigned recover_idx);
    void recover_pred_s2_ptr(unsigned recover_idx);
    void recover_pred_s3_ptr(unsigned recover_idx);

    std::vector<unsigned> get_intra_squashed_ftq_idxs(unsigned recover_idx);

    std::vector<FtqEntry> get_squash_s1_free_entries(unsigned recover_idx);
    std::vector<FtqEntry> get_squash_s2_free_entries(unsigned recover_idx);
    std::vector<FtqEntry> get_squash_s3_free_entries(unsigned recover_idx);

    FtqEntry get_commit_idx_entry();
    void commit_retire_entry();
    bool curr_entry_has_committed();
    bool next_entry_has_committed();
    bool curr_entry_should_up_bpu();

  protected:
    const unsigned ftqSize;

    FtqEntry *fetch_target_queue;

    unsigned pred_s1_idx;
    FtqEntry *pred_s1_ptr;
    unsigned pred_s2_idx;
    FtqEntry *pred_s2_ptr;
    unsigned pred_s3_idx;
    FtqEntry *pred_s3_ptr;
    unsigned fetch_idx;
    FtqEntry *fetch_ptr;
    unsigned prefetch_idx;
    FtqEntry *prefetch_ptr;
    unsigned commit_idx;
    FtqEntry *commit_ptr;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_FTQ_HH__

#include "mem/cache/prefetch/bingo.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/BingoPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Bingo::Bingo(
    const BingoPrefetcherParams &p)
    : Queued(p), pc_width(p.pc_width),
      max_addr_width(p.max_addr_width), min_addr_width(p.min_addr_width),
      region_size(p.region_size), pht_vote_threshold(p.pht_vote_threshold),
      pattern_len(p.region_size/p.block_size),
      epochCycles(p.epoch_cycles),
      filter_table(p.filter_table_assoc, p.filter_table_entry_count,
                     p.filter_table_indexing_policy,
                     p.filter_table_replacement_policy),
      accumulation_table(p.at_assoc, p.at_entry_count,
                     p.at_indexing_policy,
                     p.at_replacement_policy,
                     p.region_size/p.block_size),
      pht(p.pht_assoc, p.pht_entry_count, p.pht_indexing_policy,
                     p.pht_replacement_policy,
                     p.region_size/p.block_size, p.min_addr_width,
                     p.max_addr_width, p.pc_width),
      epochEvent([this]{ processEpochEvent(); }, name())
{
    fatal_if(!isPowerOf2(p.block_size),
        "the block size must be a power of 2");
    std::cerr<<"block_size: "<<p.block_size<<std::endl;
}

void
Bingo::startup()
{
    schedule(epochEvent, clockEdge(epochCycles));
}

void
Bingo::processEpochEvent()
{
}

void
Bingo::insert_in_phts(const AccumulationTableEntry &entry, bool is_secure)
{
    uint64_t pc = entry.pc;
    uint64_t address = entry.key * this->pattern_len + entry.offset;
    const std::vector<bool> &pattern = entry.pattern;
    this->pht.insert(pc, address, pattern, is_secure);
}

std::vector<bool>
Bingo::find_in_phts(uint64_t pc, uint64_t address, bool is_secure)
{
    std::vector<bool> pattern = this->pht.find(pc, address, is_secure);
    //uint64_t region_number = address / this->pattern_len;
    //events[region_number] = this->pht.get_last_event();
    return pattern;
}


void
Bingo::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }

    //std::cerr<<"pfi.getPC(): "<<pfi.getPC()<<std::endl;

    //uint64_t block_number = pfi.getAddr() / (uint64_t)block_size;
    uint64_t block_number = pfi.getAddr() / 64;
    Addr pc = pfi.getPC();
    bool is_secure = pfi.isSecure();
    Addr region_number = block_number / this->pattern_len;
    int region_offset = block_number % this->pattern_len;
    bool success = this->accumulation_table.set_pattern(
                    region_number, region_offset, is_secure);
    DPRINTF(HWPrefetch, "PC 0x%x, addr:0x%x, region:0x%x, offset:%d\n",
                pc, pfi.getAddr(), region_number, region_offset);
    if (success)
        return;
    FilterTableEntry *entry = this->filter_table.find(
                                region_number, is_secure);
    if (!entry) {
        /* trigger access */
        this->filter_table.insert(region_number,
                        pc, region_offset, is_secure);
        DPRINTF(HWPrefetch, "FT inserting region:0x%x, pc:0x%x, offset:%d\n%s",
                    region_number, pc, region_offset, filter_table.log());
        std::vector<bool> pattern = this->find_in_phts(pc,
                                block_number, is_secure);
        if (pattern.empty())
            return;

        for (int i = 0; i < this->pattern_len; i += 1)
            if (pattern[i]) {
                DPRINTF(HWPrefetch, "Prefetch address: 0x%x\n",
                    (region_number * this->pattern_len)*64 + i);
                addresses.push_back(AddrPriority((region_number
                                 * this->pattern_len + i)*64, 0));
            }
        return;
    }
    if (entry->offset != region_offset) {
        /* move from filter table to accumulation table */
        AccumulationTableEntry victim =
                this->accumulation_table.insert(*entry, is_secure);
        this->accumulation_table.set_pattern(region_number,
                                         region_offset, is_secure);
        DPRINTF(HWPrefetch, "AT inserting region: 0x%x,\
                                    pc: 0x%x, offset:%d\n%s",
            region_number, entry->pc, region_offset, accumulation_table.log());
        this->filter_table.erase(region_number, is_secure);
        DPRINTF(HWPrefetch, "FT erasing region: 0x%x\n", region_number);
        if (victim.key) {
            /* move from accumulation table to pht */
            this->insert_in_phts(victim, is_secure);
            DPRINTF(HWPrefetch, "PHT inserting pc: 0x%x, offset:%d\n%s",
                        victim.pc, victim.offset, pht.log());
        }
    }
    return ;
}

} // namespace prefetch
} // namespace gem5

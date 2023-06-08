#ifndef __MEM_CACHE_PREFETCH_BINGO_HH__
#define __MEM_CACHE_PREFETCH_BINGO_HH__

#include <iomanip>

#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

class BaseIndexingPolicy;
GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{
    class Base;
}
struct BingoPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class Table
{
  public:
    Table(int width, int height) : width(width), height(height),
            cells(height, std::vector<std::string>(width)) {}

    void set_row(int row, const std::vector<std::string> &data,
                        int start_col = 0) {
        assert(data.size() + start_col == this->width);
        for (unsigned col = start_col; col < this->width; col += 1)
            this->set_cell(row, col, data[col]);
    }

    void set_col(int col, const std::vector<std::string> &data,
                        int start_row = 0) {
        assert(data.size() + start_row == this->height);
        for (unsigned row = start_row; row < this->height; row += 1)
            this->set_cell(row, col, data[row]);
    }

    void set_cell(int row, int col, std::string data) {
        assert(0 <= row && row < (int)this->height);
        assert(0 <= col && col < (int)this->width);
        this->cells[row][col] = data;
    }

    void set_cell(int row, int col, double data) {
        this->oss.str("");
        this->oss << std::setw(11) << std::fixed
                << std::setprecision(8) << data;
        this->set_cell(row, col, this->oss.str());
    }

    void set_cell(int row, int col, int64_t data) {
        this->oss.str("");
        this->oss << std::setw(11) << std::left << data;
        this->set_cell(row, col, this->oss.str());
    }

    void set_cell(int row, int col, int data)
    { this->set_cell(row, col, (int64_t)data); }

    void set_cell(int row, int col, uint64_t data)
    { this->set_cell(row, col, (int64_t)data); }

    std::string to_string() {
        std::vector<int> widths;
        for (unsigned i = 0; i < this->width; i += 1) {
            int max_width = 0;
            for (unsigned j = 0; j < this->height; j += 1)
                max_width = std::max(max_width,
                        (int)this->cells[j][i].size());
            widths.push_back(max_width + 2);
        }
        std::string out;
        out += Table::top_line(widths);
        out += this->data_row(0, widths);
        for (unsigned i = 1; i < this->height; i += 1) {
            out += Table::mid_line(widths);
            out += this->data_row(i, widths);
        }
        out += Table::bot_line(widths);
        return out;
    }

    std::string data_row(int row, const std::vector<int> &widths) {
        std::string out;
        for (unsigned i = 0; i < this->width; i += 1) {
            std::string data = this->cells[row][i];
            data.resize(widths[i] - 2, ' ');
            out += " | " + data;
        }
        out += " |\n";
        return out;
    }

    static std::string top_line(const std::vector<int> &widths)
    { return Table::line(widths, "┌", "┬", "┐"); }

    static std::string mid_line(const std::vector<int> &widths)
    { return Table::line(widths, "├", "┼", "┤"); }

    static std::string bot_line(const std::vector<int> &widths)
    { return Table::line(widths, "└", "┴", "┘"); }

    static std::string line(const std::vector<int> &widths,
        std::string left, std::string mid, std::string right) {
        std::string out = " " + left;
        for (unsigned i = 0; i < widths.size(); i += 1) {
            int w = widths[i];
            for (int j = 0; j < w; j += 1)
                out += "─";
            if (i != widths.size() - 1)
                out += mid;
            else
                out += right;
        }
        return out + "\n";
    }

  private:
    unsigned width;
    unsigned height;
    std::vector<std::vector<std::string>> cells;
    std::ostringstream oss;
};


class FilterTableEntry  : public TaggedEntry
{
  public:
    Addr key;
    Addr pc;
    int offset;
    FilterTableEntry()
        :TaggedEntry(), key(0), pc(0), offset(0) { }
};

class FilterTable
{
 private:
   AssociativeSet<FilterTableEntry> ft_cache;

  public:
    FilterTable(int filter_table_assoc, int filter_table_entry_count,
                BaseIndexingPolicy*  filter_table_indexing_policy,
                replacement_policy::Base*  filter_table_replacement_policy)
                : ft_cache(filter_table_assoc, filter_table_entry_count,
                  filter_table_indexing_policy,
                  filter_table_replacement_policy)
        { assert(__builtin_popcount(filter_table_entry_count) == 1); }

    FilterTableEntry *find(Addr region_number, bool is_secure) {
        FilterTableEntry *entry = ft_cache.findEntry(region_number, is_secure);
        if (!entry)
            return nullptr;
        ft_cache.accessEntry(entry);
        return entry;
    }

    void insert(Addr region_number, Addr pc, int offset, bool is_secure) {
        assert(!ft_cache.findEntry(region_number, is_secure));
        FilterTableEntry *entry = ft_cache.findVictim(region_number);
        assert(entry != nullptr);
        entry->key = region_number;
        entry->pc = pc;
        entry->offset = offset;
        ft_cache.insertEntry(region_number, is_secure, entry);
        //ft_cache.accessEntry(entry);
    }

    void erase(Addr region_number, bool is_secure){
        FilterTableEntry *entry = ft_cache.findEntry(region_number, is_secure);
        assert(entry);
        ft_cache.invalidate(entry);
    }

    static void write_data(FilterTableEntry &entry, Table &table, int row) {
        table.set_cell(row, 0, entry.key);
        std::string map = "";
        //for (unsigned i = 0; i < entry.data.pattern.size(); i += 1){
        std::ostringstream os,os1;
        os << std::hex;
        os << entry.pc;
        map += std::string(os.str());
        map += "/";
        os1 << entry.offset;
        map += std::string(os1.str());
        //}
        table.set_cell(row, 1, map);
    }

    std::vector<FilterTableEntry> get_valid_entries() {
        std::vector<FilterTableEntry> valid_entries;
        for (auto iter = ft_cache.begin(); iter != ft_cache.end(); iter++)
                if ((*iter).isValid())
                    valid_entries.push_back(*iter);
        return valid_entries;
    }

    std::string log() {
        std::vector<std::string> headers({"FT Tag", "pc/offset"});
        std::vector<FilterTableEntry> valid_entries = get_valid_entries();
        Table table(headers.size(), valid_entries.size() + 1);
        table.set_row(0, headers);
        for (unsigned i = 0; i < valid_entries.size(); i += 1)
            write_data(valid_entries[i], table, i + 1);

        return table.to_string();
    }



};

class AccumulationTableEntry : public TaggedEntry
{
  public:
    Addr key;
    Addr pc;
    int offset;
    std::vector<bool> pattern;
    AccumulationTableEntry()
    :TaggedEntry(), key(0), pc(0), offset(0), pattern({0}){}
};

class AccumulationTable
{
    unsigned pattern_len;
    AssociativeSet<AccumulationTableEntry> at_cache;

  public:
    AccumulationTable(int accumulation_table_assoc,
         int accumulation_table_entry_count,
         BaseIndexingPolicy* accumulation_table_indexing_policy,
         replacement_policy::Base* accumulation_table_replacement_policy,
         unsigned p_len)
        :pattern_len(p_len),
         at_cache(accumulation_table_assoc, accumulation_table_entry_count,
         accumulation_table_indexing_policy,
         accumulation_table_replacement_policy)
     {
        assert(__builtin_popcount(accumulation_table_entry_count) == 1);
        assert(__builtin_popcount(pattern_len) == 1);
    }

    /**
     * @return A return value of false means that
     * the tag wasn't found in the table and true means success.
     */
    bool set_pattern(Addr region_number, int offset, bool is_secure) {
        AccumulationTableEntry *entry =
                at_cache.findEntry(region_number, is_secure);
        if (!entry)
            return false;
        entry->pattern[offset] = true;
        at_cache.accessEntry(entry);
        return true;
    }

    AccumulationTableEntry insert(FilterTableEntry &entry, bool is_secure) {
        assert(!at_cache.findEntry(entry.key, is_secure));
        std::vector<bool> pattern(this->pattern_len, false);
        pattern[entry.offset] = true;
        AccumulationTableEntry* victim = at_cache.findVictim(entry.key);
        assert(victim != nullptr);
        AccumulationTableEntry old_entry = *victim;
        victim->key = entry.key;
        victim->pc = entry.pc;
        victim->offset = entry.offset;
        victim->pattern = pattern;
        assert(victim->replacementData != 0);
        at_cache.insertEntry(entry.key, is_secure, victim);
        return old_entry;
    }

    static void write_data(AccumulationTableEntry &entry,
                    Table &table, int row) {
        table.set_cell(row, 0, entry.key);
        std::string map = "";
        std::ostringstream os,os1;
        os << std::hex;
        os << entry.pc;
        map += std::string(os.str());
        map += "/";
        os << std::hex;
        os1 << entry.offset;
        map += std::string(os1.str());
        map += "/";

        for (unsigned i = 0; i < entry.pattern.size(); i += 1){
            std::ostringstream os2;
            os2 << entry.pattern[i];
            map += std::string(os2.str());
        }
        table.set_cell(row, 1, map);
    }

    std::vector<AccumulationTableEntry> get_valid_entries() {
        std::vector<AccumulationTableEntry> valid_entries;
        for (auto iter = at_cache.begin(); iter != at_cache.end(); iter++)
                if ((*iter).isValid())
                    valid_entries.push_back(*iter);
        return valid_entries;
    }

    std::string log() {
        std::vector<std::string> headers({"AT Tag", "pc/offset/pattern"});
        std::vector<AccumulationTableEntry> valid_entries
                         = get_valid_entries();
        Table table(headers.size(), valid_entries.size() + 1);
        table.set_row(0, headers);
        for (unsigned i = 0; i < valid_entries.size(); i += 1)
            write_data(valid_entries[i], table, i + 1);

        return table.to_string();
    }
};

class PatternHistoryTableEntry : public TaggedEntry
{
  public:
    Addr key;
    std::vector<bool> pattern;
    PatternHistoryTableEntry()
                :TaggedEntry(), key(0), pattern({0}){}
};
class PatternHistoryTable
{
  private:
    //Event last_event;
    unsigned pattern_len, min_addr_width, max_addr_width, pc_width, index_len;
    int num_sets;
    AssociativeSet<PatternHistoryTableEntry> pht_cache;
  public:
    PatternHistoryTable(int pht_assoc,
                     int pht_entry_count,
                     BaseIndexingPolicy*  pht_indexing_policy,
                     replacement_policy::Base*  pht_replacement_policy,
                     unsigned pattern_len, unsigned min_addr_width,
                     unsigned max_addr_width, unsigned pc_width)
        :pattern_len(pattern_len), min_addr_width(min_addr_width),
            max_addr_width(max_addr_width), pc_width(pc_width),
            num_sets(pht_entry_count/pht_assoc),
            pht_cache(pht_assoc, pht_entry_count,
                pht_indexing_policy,
                pht_replacement_policy)

    {
        assert(this->max_addr_width >= this->min_addr_width);
        assert(this->pc_width + this->min_addr_width > 0);
        assert(__builtin_popcount(pattern_len) == 1);
        this->index_len = __builtin_ctz(num_sets);
    }

    /* address is actually block number */
    void insert(Addr pc, Addr address,
                std::vector<bool> pattern, bool is_secure) {
        assert((int)pattern.size() == this->pattern_len);
        //int offset = address % this->pattern_len;
        //pattern = my_rotate(pattern, -offset);
        Addr key = this->build_key(pc, address);
        PatternHistoryTableEntry *new_entry = pht_cache.findVictim(key);
        assert(new_entry != nullptr);
        new_entry->key = key;
        new_entry->pattern = pattern;
        pht_cache.insertEntry(key, is_secure, new_entry);
    }


    /**
     * @return An un-rotated pattern if match was found,
     * otherwise an empty std::vector.
     * Finds best match and in case of ties, uses the MRU entry.
     */
    std::vector<bool> find(Addr pc, Addr address, bool is_secure) {
        Addr key = this->build_key(pc, address);
        //Addr index = key % this->num_sets;
        Addr tag = key / this->num_sets;
        //auto &set = this->entries[index];
        std::vector<PatternHistoryTableEntry*> set =
                pht_cache.getPossibleEntries(key);
        Addr min_tag_mask = (1 << (this->pc_width + this->min_addr_width
                                         - this->index_len)) - 1;
        Addr max_tag_mask = (1 << (this->pc_width + this->max_addr_width
                                         - this->index_len)) - 1;
        std::vector<std::vector<bool>> min_matches;
        std::vector<bool> pattern;
        for (const auto& location : set) {
            PatternHistoryTableEntry* entry =
                        static_cast<PatternHistoryTableEntry *>(location);
            if (!entry->isValid())
                continue;
            bool min_match = ((entry->getTag() & min_tag_mask)
                                 == (tag & min_tag_mask))
                                && entry->isSecure() == is_secure;
            bool max_match = ((entry->getTag() & max_tag_mask)
                                 == (tag & max_tag_mask))
                                && entry->isSecure() == is_secure;
            std::vector<bool> &cur_pattern = entry->pattern;
            if (max_match) {
                pht_cache.accessEntry(entry);
                pattern = cur_pattern;
                break;
            }
            if (min_match) {
                min_matches.push_back(cur_pattern);
            }
        }
        //this->last_event = PC_ADDRESS;
        if (pattern.empty()) {
            /* no max match was found, time for a vote! */
            pattern = this->vote(min_matches);
            //this->last_event = PC_OFFSET;
        }
        //int offset = address % this->pattern_len;
        //pattern = my_rotate(pattern, +offset);
        return pattern;
    }




    static void write_data(PatternHistoryTableEntry &entry,
                    Table &table, int row) {
        table.set_cell(row, 0, entry.key);
        std::string map = "";
        std::ostringstream os,os1;
        os << std::hex;
        os << entry.key;
        map += std::string(os.str());
        map += "/";

        for (unsigned i = 0; i < entry.pattern.size(); i += 1){
            std::ostringstream os2;
            os2 << entry.pattern[i];
            map += std::string(os2.str());
        }
        table.set_cell(row, 1, map);
    }

    std::vector<PatternHistoryTableEntry> get_valid_entries() {
        std::vector<PatternHistoryTableEntry> valid_entries;
        for (auto iter = pht_cache.begin(); iter != pht_cache.end(); iter++)
                if ((*iter).isValid())
                    valid_entries.push_back(*iter);
        return valid_entries;
    }

    std::string log() {
        std::vector<std::string> headers({"PHT Tag", "key/pattern"});
        std::vector<PatternHistoryTableEntry> valid_entries
                            = get_valid_entries();
        Table table(headers.size(), valid_entries.size() + 1);
        table.set_row(0, headers);
        for (unsigned i = 0; i < valid_entries.size(); i += 1)
            write_data(valid_entries[i], table, i + 1);

        return table.to_string();
    }

    //Event get_last_event() {
    //    return this->last_event;
    //}

  private:
    Addr build_key(Addr pc, Addr address) {
        /* use [pc_width] bits from pc */
        pc &= (1 << this->pc_width) - 1;
        /* use [addr_width] bits from address */
        address &= (1 << this->max_addr_width) - 1;
        Addr offset = address & ((1 << this->min_addr_width) - 1);
        Addr base = (address >> this->min_addr_width);
        /* base + pc + offset */
        Addr key = (base << (this->pc_width + this->min_addr_width))
                        | (pc << this->min_addr_width) | offset;
        /* CRC */
        Addr tag = ((pc << this->min_addr_width) | offset);
        do {
            tag >>= this->index_len;
            key ^= tag & ((1 << this->index_len) - 1);
        } while (tag > 0);
        return key;
    }

    std::vector<bool> vote(const std::vector<std::vector<bool>> &x,
                float thresh = 0.2) {
        int n = x.size();
        std::vector<bool> ret(this->pattern_len, false);
        for (int i = 0; i < n; i += 1)
            assert((int)x[i].size() == this->pattern_len);
        for (int i = 0; i < this->pattern_len; i += 1) {
            int cnt = 0;
            for (int j = 0; j < n; j += 1)
                if (x[j][i])
                    cnt += 1;
            if (1.0 * cnt / n >= thresh)
                ret[i] = true;
        }
        return ret;
    }


};

class Bingo : public Queued
{
    private:
        /** Cacheline size used by the prefetcher using this object */
        //const unsigned block_size;
        const unsigned pc_width;
        const unsigned max_addr_width;
        const unsigned min_addr_width;
        const unsigned region_size;
        const float    pht_vote_threshold;
        const unsigned pattern_len;
         /** Cycles in an epoch period */
        const Cycles epochCycles;

        /** Access map table */
        FilterTable  filter_table;
        AccumulationTable accumulation_table;
        PatternHistoryTable pht;

        /** search pht to find the history pattern of one region number
         *  indexed by pc and address
         * @param pc: the program counter of the memroy access
         * @param address: the memory access address
         */
        std::vector<bool> find_in_phts(Addr pc, Addr address, bool is_secure);

        /**
         * insert one entry into pht         *
         * @param entry: come from accumulation table
         * as a victim or eviction entry
         */
        void insert_in_phts(const AccumulationTableEntry &entry,
                                bool is_secure);


        /**
         * This event constitues the epoch of the statistics that keep track of
         * the prefetcher accuracy, when this event triggers,
         * the prefetcher degree is adjusted and
         * the statistics counters are reset.
         */
        void processEpochEvent();
        EventFunctionWrapper epochEvent;

    public:
        Bingo(const BingoPrefetcherParams &p);
        ~Bingo() = default;

        void startup() override;
        void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
        //void eviction(Addr block_number);
};

} // namespace prefetch
} // namespace gem5
#endif //__MEM_CACHE_PREFETCH_BINGO_HH__

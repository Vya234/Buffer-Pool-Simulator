#ifndef DEFS_H
#define DEFS_H

#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define PAGE_SIZE 512

class Page;

struct hash_struct {
    unsigned long operator()(const std::pair<FILE*, int>& p) const {
        const size_t file_hash = std::hash<FILE*>{}(p.first);
        const size_t int_hash = std::hash<int>{}(p.second);
        return file_hash ^ (int_hash << 1);
    }
};

struct StepInfo {
    bool hit = false;
    int requested_page = -1;
    int evicted_page = -1;
    bool dirty_eviction = false;
    bool buffer_full_pinned = false;
    std::string eviction_reason;
    std::vector<int> buffer_state;
    int disk_reads = 0;
    int disk_writes = 0;
};

struct SimResult {
    std::string strategy;
    std::string workload;
    int buffer_size = 0;
    int accesses = 0;
    int disk_reads = 0;
    int disk_writes = 0;
    int total_io = 0;
    int hits = 0;
    int misses = 0;
    float hit_rate = 0.0f;
    int evictions = 0;
};

class clock_buffer_manager {
private:
    FILE* log_ptr;
    int clock_hand;
    unsigned int num_bufs;
    int buf_cnt;
    std::unordered_map<std::pair<FILE*, int>, int, hash_struct> buf_map;
    Page* buf_pool;
    long long access_tick;
    int choose_victim_clock(StepInfo* step);
    Page* load_page_into_slot(FILE* f, int page_number, int idx, StepInfo* step);

public:
    int accesses, disk_reads, disk_writes, hits, misses, evictions;
    clock_buffer_manager(unsigned int n);
    ~clock_buffer_manager();
    Page* read_page(FILE* f, int page_number);
    Page* access_page(int page_number, bool is_write = false, StepInfo* step = nullptr);
    void mark_dirty(Page* p);
    void unpin_page(Page* p);
    void reset_stats();
    std::vector<int> get_buffer_state() const;
};

class lru_buffer_manager {
private:
    FILE* log_ptr;
    unsigned int num_bufs;
    int buf_cnt;
    long long access_tick;
    std::list<Page*> buf_pool;
    std::vector<Page*> frame_slots;
    std::unordered_map<std::pair<FILE*, int>, std::list<Page*>::iterator, hash_struct> buf_map;
    std::list<Page*>::iterator find_lru_unpinned();
    Page* read_or_map_page(FILE* fptr, int page_number, bool is_write, StepInfo* step);

public:
    int accesses, disk_reads, disk_writes, hits, misses, evictions;
    lru_buffer_manager(unsigned int n);
    ~lru_buffer_manager();
    Page* read_page(FILE* f, int page_number);
    Page* access_page(int page_number, bool is_write = false, StepInfo* step = nullptr);
    void mark_dirty(Page* p);
    void unpin_page(Page* p);
    void reset_stats();
    std::vector<int> get_buffer_state() const;
};

class mru_buffer_manager {
private:
    FILE* log_ptr;
    unsigned int num_bufs;
    int buf_cnt;
    long long access_tick;
    std::list<Page*> buf_pool;
    std::vector<Page*> frame_slots;
    std::unordered_map<std::pair<FILE*, int>, std::list<Page*>::iterator, hash_struct> buf_map;
    std::list<Page*>::iterator find_mru_unpinned();
    Page* read_or_map_page(FILE* fptr, int page_number, bool is_write, StepInfo* step);

public:
    int accesses, disk_reads, disk_writes, hits, misses, evictions;
    mru_buffer_manager(unsigned int n);
    ~mru_buffer_manager();
    Page* read_page(FILE* f, int page_number);
    Page* access_page(int page_number, bool is_write = false, StepInfo* step = nullptr);
    void mark_dirty(Page* p);
    void unpin_page(Page* p);
    void reset_stats();
    std::vector<int> get_buffer_state() const;
};

class Page {
private:
    FILE* fptr;
    int page_num;
    bool ref_bit;
    bool pin;
    bool dirty;
    bool valid;
    int frame_index;
    long long last_access_tick;
    Page();
    ~Page();

public:
    void* disk_block;
    friend class clock_buffer_manager;
    friend class lru_buffer_manager;
    friend class mru_buffer_manager;
};

#endif
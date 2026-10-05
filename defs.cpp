#include "defs.h"

#include <cstring>
#include <iostream>

namespace {
std::pair<FILE*, int> make_key(FILE* f, int page_number) {
    return std::make_pair(f, page_number);
}

void fill_step(StepInfo* step, const std::vector<int>& state, int reads, int writes) {
    if (!step) {
        return;
    }
    step->buffer_state = state;
    step->disk_reads = reads;
    step->disk_writes = writes;
}
}  // namespace

Page::Page()
    : fptr(nullptr),
      page_num(-1),
      ref_bit(false),
      pin(false),
      dirty(false),
      valid(false),
      frame_index(-1),
      last_access_tick(0) {
    disk_block = std::malloc(PAGE_SIZE);
}

Page::~Page() {
    std::free(disk_block);
}

clock_buffer_manager::clock_buffer_manager(unsigned int n)
    : log_ptr(nullptr),
      clock_hand(n == 0 ? 0 : static_cast<int>(n) - 1),
      num_bufs(n),
      buf_cnt(0),
      buf_pool(nullptr),
      access_tick(0),
      accesses(0),
      disk_reads(0),
      disk_writes(0),
      hits(0),
      misses(0),
      evictions(0) {
    buf_pool = new Page[n];
    log_ptr = std::fopen("log_clock.txt", "w");
}

clock_buffer_manager::~clock_buffer_manager() {
    delete[] buf_pool;
    if (log_ptr) {
        std::fclose(log_ptr);
    }
}

void clock_buffer_manager::mark_dirty(Page* p) {
    if (p) {
        p->dirty = true;
    }
}

void clock_buffer_manager::unpin_page(Page* p) {
    if (p) {
        p->pin = false;
    }
}

void clock_buffer_manager::reset_stats() {
    accesses = 0;
    disk_reads = 0;
    disk_writes = 0;
    hits = 0;
    misses = 0;
    evictions = 0;
}

std::vector<int> clock_buffer_manager::get_buffer_state() const {
    std::vector<int> state;
    state.reserve(num_bufs);
    for (unsigned int i = 0; i < num_bufs; ++i) {
        state.push_back(buf_pool[i].valid ? buf_pool[i].page_num : -1);
    }
    return state;
}

int clock_buffer_manager::choose_victim_clock(StepInfo* step) {
    if (num_bufs == 0) {
        return -1;
    }

    unsigned int checked = 0;
    while (checked < 2 * num_bufs) {
        const int idx = (clock_hand + 1) % static_cast<int>(num_bufs);
        clock_hand = idx;
        ++checked;
        if (!buf_pool[idx].valid || buf_pool[idx].pin) {
            continue;
        }
        if (buf_pool[idx].ref_bit) {
            buf_pool[idx].ref_bit = false;
            continue;
        }
        if (!buf_pool[idx].ref_bit) {
            clock_hand = idx;
            if (step) {
                step->eviction_reason = "CLOCK";
            }
            return idx;
        }
    }

    if (step) {
        step->buffer_full_pinned = true;
    }
    return -1;
}

Page* clock_buffer_manager::load_page_into_slot(FILE* f, int page_number, int idx, StepInfo* step) {
    if (idx < 0) {
        return nullptr;
    }

    Page& slot = buf_pool[idx];
    if (slot.valid) {
        ++evictions;
        if (step) {
            step->evicted_page = slot.page_num;
        }
        if (slot.dirty) {
            // Dirty eviction: write before loading new page.
            ++disk_writes;
            if (step) {
                step->dirty_eviction = true;
            }
        }
        buf_map.erase(make_key(slot.fptr, slot.page_num));
    }

    // Every miss loads one page from disk.
    ++disk_reads;
    ++misses;

    slot.fptr = f;
    slot.page_num = page_number;
    slot.pin = true;
    slot.ref_bit = true;
    slot.dirty = false;
    slot.valid = true;
    slot.last_access_tick = ++access_tick;
    std::memset(slot.disk_block, 0, PAGE_SIZE);

    buf_map[make_key(f, page_number)] = idx;
    fill_step(step, get_buffer_state(), disk_reads, disk_writes);
    return &slot;
}

Page* clock_buffer_manager::read_page(FILE* f, int page_number) {
    if (f == nullptr || page_number < 0) {
        return nullptr;
    }

    StepInfo ignored;
    ++accesses;
    auto key = make_key(f, page_number);
    auto it = buf_map.find(key);
    if (it != buf_map.end()) {
        ++hits;
        Page& p = buf_pool[it->second];
        p.pin = true;
        p.ref_bit = true;
        p.last_access_tick = ++access_tick;
        fill_step(&ignored, get_buffer_state(), disk_reads, disk_writes);
        return &p;
    }

    if (buf_cnt < static_cast<int>(num_bufs)) {
        int idx = buf_cnt++;
        return load_page_into_slot(f, page_number, idx, &ignored);
    }

    int victim = choose_victim_clock(&ignored);
    if (victim < 0) {
        if (log_ptr) {
            std::fprintf(log_ptr, "BUFFER FULL: All frames pinned, cannot evict.\n");
        }
        return nullptr;
    }

    return load_page_into_slot(f, page_number, victim, &ignored);
}

Page* clock_buffer_manager::access_page(int page_number, bool is_write, StepInfo* step) {
    if (page_number < 0) {
        return nullptr;
    }
    if (step) {
        *step = StepInfo{};
        step->requested_page = page_number;
    }

    FILE* synthetic_file = nullptr;
    ++accesses;
    auto key = make_key(synthetic_file, page_number);
    auto it = buf_map.find(key);
    if (it != buf_map.end()) {
        Page& p = buf_pool[it->second];
        p.pin = true;
        p.ref_bit = true;
        p.last_access_tick = ++access_tick;
        if (is_write) {
            p.dirty = true;
        }
        ++hits;
        if (step) {
            step->hit = true;
        }
        fill_step(step, get_buffer_state(), disk_reads, disk_writes);
        return &p;
    }

    if (buf_cnt < static_cast<int>(num_bufs)) {
        int idx = buf_cnt++;
        Page* loaded = load_page_into_slot(synthetic_file, page_number, idx, step);
        if (loaded && is_write) {
            loaded->dirty = true;
        }
        return loaded;
    }

    int victim = choose_victim_clock(step);
    if (victim < 0) {
        if (step) {
            step->buffer_full_pinned = true;
        }
        if (log_ptr) {
            std::fprintf(log_ptr, "BUFFER FULL: All frames pinned, cannot evict.\n");
        }
        fill_step(step, get_buffer_state(), disk_reads, disk_writes);
        return nullptr;
    }

    Page* loaded = load_page_into_slot(synthetic_file, page_number, victim, step);
    if (loaded && is_write) {
        loaded->dirty = true;
    }
    return loaded;
}

lru_buffer_manager::lru_buffer_manager(unsigned int n)
    : log_ptr(nullptr),
      num_bufs(n),
      buf_cnt(0),
      access_tick(0),
      frame_slots(n, nullptr),
      accesses(0),
      disk_reads(0),
      disk_writes(0),
      hits(0),
      misses(0),
      evictions(0) {
    log_ptr = std::fopen("log_lru.txt", "w");
}

lru_buffer_manager::~lru_buffer_manager() {
    for (Page* page : buf_pool) {
        delete page;
    }
    if (log_ptr) {
        std::fclose(log_ptr);
    }
}

void lru_buffer_manager::mark_dirty(Page* p) {
    if (p) {
        p->dirty = true;
    }
}

void lru_buffer_manager::unpin_page(Page* p) {
    if (p) {
        p->pin = false;
    }
}

void lru_buffer_manager::reset_stats() {
    accesses = 0;
    disk_reads = 0;
    disk_writes = 0;
    hits = 0;
    misses = 0;
    evictions = 0;
}

std::vector<int> lru_buffer_manager::get_buffer_state() const {
    std::vector<int> state;
    state.reserve(num_bufs);
    for (Page* page : frame_slots) {
        state.push_back((page && page->valid) ? page->page_num : -1);
    }
    return state;
}

std::list<Page*>::iterator lru_buffer_manager::find_lru_unpinned() {
    auto best = buf_pool.end();
    long long best_tick = 0;
    for (auto it = buf_pool.begin(); it != buf_pool.end(); ++it) {
        if ((*it)->pin) {
            continue;
        }
        if (best == buf_pool.end() || (*it)->last_access_tick < best_tick) {
            best = it;
            best_tick = (*it)->last_access_tick;
        }
    }
    return best;
}

Page* lru_buffer_manager::read_or_map_page(FILE* fptr, int page_number, bool is_write, StepInfo* step) {
    ++accesses;
    auto key = make_key(fptr, page_number);
    auto map_it = buf_map.find(key);
    if (map_it != buf_map.end()) {
        Page* page = *map_it->second;
        page->pin = true;
        page->ref_bit = true;
        page->last_access_tick = ++access_tick;
        if (is_write) {
            page->dirty = true;
        }
        buf_pool.erase(map_it->second);
        buf_pool.push_front(page);
        buf_map[key] = buf_pool.begin();
        ++hits;
        if (step) {
            step->hit = true;
        }
        fill_step(step, get_buffer_state(), disk_reads, disk_writes);
        return page;
    }

    Page* page = nullptr;

    if (buf_cnt < static_cast<int>(num_bufs)) {
        page = new Page();
        page->frame_index = buf_cnt;
        frame_slots[page->frame_index] = page;
        ++buf_cnt;
    } else {
        auto victim_it = find_lru_unpinned();
        if (victim_it == buf_pool.end()) {
            if (step) {
                step->buffer_full_pinned = true;
            }
            if (log_ptr) {
                std::fprintf(log_ptr, "BUFFER FULL: All frames pinned, cannot evict.\n");
            }
            fill_step(step, get_buffer_state(), disk_reads, disk_writes);
            return nullptr;
        }
        page = *victim_it;
        ++evictions;
        if (step) {
            step->eviction_reason = "LRU";
            step->evicted_page = page->page_num;
        }
        if (page->dirty) {
            ++disk_writes;
            if (step) {
                step->dirty_eviction = true;
            }
        }
        buf_map.erase(make_key(page->fptr, page->page_num));
        buf_pool.erase(victim_it);
    }

    page->fptr = fptr;
    page->page_num = page_number;
    page->pin = true;
    page->ref_bit = true;
    page->dirty = is_write;
    page->valid = true;
    page->last_access_tick = ++access_tick;
    ++misses;
    ++disk_reads;
    std::memset(page->disk_block, 0, PAGE_SIZE);

    buf_pool.push_front(page);
    buf_map[key] = buf_pool.begin();
    fill_step(step, get_buffer_state(), disk_reads, disk_writes);
    return page;
}

Page* lru_buffer_manager::read_page(FILE* f, int page_number) {
    if (f == nullptr || page_number < 0) {
        return nullptr;
    }
    return read_or_map_page(f, page_number, false, nullptr);
}

Page* lru_buffer_manager::access_page(int page_number, bool is_write, StepInfo* step) {
    if (page_number < 0) {
        return nullptr;
    }
    if (step) {
        *step = StepInfo{};
        step->requested_page = page_number;
    }
    return read_or_map_page(nullptr, page_number, is_write, step);
}

mru_buffer_manager::mru_buffer_manager(unsigned int n)
    : log_ptr(nullptr),
      num_bufs(n),
      buf_cnt(0),
      access_tick(0),
      frame_slots(n, nullptr),
      accesses(0),
      disk_reads(0),
      disk_writes(0),
      hits(0),
      misses(0),
      evictions(0) {
    log_ptr = std::fopen("log_mru.txt", "w");
}

mru_buffer_manager::~mru_buffer_manager() {
    for (Page* page : buf_pool) {
        delete page;
    }
    if (log_ptr) {
        std::fclose(log_ptr);
    }
}

void mru_buffer_manager::mark_dirty(Page* p) {
    if (p) {
        p->dirty = true;
    }
}

void mru_buffer_manager::unpin_page(Page* p) {
    if (p) {
        p->pin = false;
    }
}

void mru_buffer_manager::reset_stats() {
    accesses = 0;
    disk_reads = 0;
    disk_writes = 0;
    hits = 0;
    misses = 0;
    evictions = 0;
}

std::vector<int> mru_buffer_manager::get_buffer_state() const {
    std::vector<int> state;
    state.reserve(num_bufs);
    for (Page* page : frame_slots) {
        state.push_back((page && page->valid) ? page->page_num : -1);
    }
    return state;
}

std::list<Page*>::iterator mru_buffer_manager::find_mru_unpinned() {
    auto best = buf_pool.end();
    long long best_tick = 0;
    for (auto it = buf_pool.begin(); it != buf_pool.end(); ++it) {
        if ((*it)->pin) {
            continue;
        }
        if (best == buf_pool.end() || (*it)->last_access_tick > best_tick) {
            best = it;
            best_tick = (*it)->last_access_tick;
        }
    }
    return best;
}

Page* mru_buffer_manager::read_or_map_page(FILE* fptr, int page_number, bool is_write, StepInfo* step) {
    ++accesses;
    auto key = make_key(fptr, page_number);
    auto map_it = buf_map.find(key);
    if (map_it != buf_map.end()) {
        Page* page = *map_it->second;
        page->pin = true;
        page->ref_bit = true;
        page->last_access_tick = ++access_tick;
        if (is_write) {
            page->dirty = true;
        }
        buf_pool.erase(map_it->second);
        buf_pool.push_front(page);
        buf_map[key] = buf_pool.begin();
        ++hits;
        if (step) {
            step->hit = true;
        }
        fill_step(step, get_buffer_state(), disk_reads, disk_writes);
        return page;
    }

    Page* page = nullptr;

    if (buf_cnt < static_cast<int>(num_bufs)) {
        page = new Page();
        page->frame_index = buf_cnt;
        frame_slots[page->frame_index] = page;
        ++buf_cnt;
    } else {
        auto victim_it = find_mru_unpinned();
        if (victim_it == buf_pool.end()) {
            if (step) {
                step->buffer_full_pinned = true;
            }
            if (log_ptr) {
                std::fprintf(log_ptr, "BUFFER FULL: All frames pinned, cannot evict.\n");
            }
            fill_step(step, get_buffer_state(), disk_reads, disk_writes);
            return nullptr;
        }
        page = *victim_it;
        ++evictions;
        if (step) {
            step->eviction_reason = "MRU";
            step->evicted_page = page->page_num;
        }
        if (page->dirty) {
            ++disk_writes;
            if (step) {
                step->dirty_eviction = true;
            }
        }
        buf_map.erase(make_key(page->fptr, page->page_num));
        buf_pool.erase(victim_it);
    }

    page->fptr = fptr;
    page->page_num = page_number;
    page->pin = true;
    page->ref_bit = true;
    page->dirty = is_write;
    page->valid = true;
    page->last_access_tick = ++access_tick;
    ++misses;
    ++disk_reads;
    std::memset(page->disk_block, 0, PAGE_SIZE);

    buf_pool.push_front(page);
    buf_map[key] = buf_pool.begin();
    fill_step(step, get_buffer_state(), disk_reads, disk_writes);
    return page;
}

Page* mru_buffer_manager::read_page(FILE* f, int page_number) {
    if (f == nullptr || page_number < 0) {
        return nullptr;
    }
    return read_or_map_page(f, page_number, false, nullptr);
}

Page* mru_buffer_manager::access_page(int page_number, bool is_write, StepInfo* step) {
    if (page_number < 0) {
        return nullptr;
    }
    if (step) {
        *step = StepInfo{};
        step->requested_page = page_number;
    }
    return read_or_map_page(nullptr, page_number, is_write, step);
}
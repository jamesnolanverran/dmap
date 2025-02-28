#include "dmap.h"
#include <stdlib.h> 
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef DMAP_DEBUG
    #if defined(_MSC_VER) || defined(_WIN32)
        #include <intrin.h>  
        #define DEBUGBREAK() __debugbreak()
    #else
        #define DEBUGBREAK() __builtin_trap()
    #endif

    #ifndef assert
        #define assert(expr)                                   \
            do {                                               \
                if (!(expr)) {                                 \
                    common_assert_failed(__FILE__, __LINE__);  \
                }                                              \
            } while (0)
    #endif // assert

    #ifndef common_assert_failed
        #define common_assert_failed(f, l)                     \
            do {                                               \
                printf("assert failed at file %s(%d)", f, l);  \
                DEBUGBREAK();                                  \
            } while (0)
    #endif // common_assert_failed
#else
    // #include <assert.h>
    #define assert
#endif

#if defined(_MSC_VER) || defined(_WIN32)
    #include <intrin.h>  // MSVC intrinsics
#endif

static inline size_t next_power_of_2(size_t x) {
    if (x <= 1) return 1;  // Ensure minimum value of 1

    #if defined(_MSC_VER) || defined(_WIN32)
        unsigned long index;
        if (_BitScanReverse64(&index, x - 1)) {
            return 1ULL << (index + 1);
        }
    #else
        return 1ULL << (64 - __builtin_clzl(x - 1));
    #endif

    return 1;
}

#include <time.h>
#if defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
#endif
#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#endif
// todo: needed?
// random hash seed 
uint64_t dmap_generate_seed() {
    uint64_t seed = 14695981039346656037ULL; 
    uint64_t timestamp = 0;
    #ifdef _WIN32
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        uint64_t pid = (uint64_t)_getpid();
    #else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        timestamp = ((uint64_t)ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
        uint64_t pid = (uint64_t)getpid();
    #endif

    seed ^= timestamp;
    seed *= 1099511628211ULL;
    seed ^= pid;
    seed *= 1099511628211ULL;

    return seed;
}

#ifndef MAX
#define MAX(x, y) ((x) >= (y) ? (x) : (y))
#endif

#define ALIGN_DOWN(n, a) ((n) & ~((a) - 1))
#define ALIGN_UP(n, a) ALIGN_DOWN((n) + (a) - 1, (a))

#define ALIGN_DOWN_PTR(p, a) ((void *)ALIGN_DOWN((uintptr_t)(p), (a)))
#define ALIGN_UP_PTR(p, a) ((void *)ALIGN_UP((uintptr_t)(p), (a)))

typedef int8_t      s8; 
typedef int16_t     s16;
typedef int32_t     s32;
typedef int64_t     s64;
typedef uint8_t     u8; 
typedef uint16_t    u16;
typedef uint32_t    u32;
typedef uint64_t    u64;

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: ERR HANDLER
// /////////////////////////////////////////////
// /////////////////////////////////////////////

// todo: improve default error handler 
static void dmap_default_error_handler(char* err_msg) {
    perror(err_msg);
    exit(1); 
}
static void (*dmap_error_handler)(char* err_msg) = dmap_default_error_handler;

void dmap_set_error_handler(void (*handler)(char* err_msg)) {
    dmap_error_handler = handler ? handler : dmap_default_error_handler; // fallback to default
}
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// PLATFORM ALLOCATOR IMPL
// /////////////////////////////////////////////
// /////////////////////////////////////////////

typedef struct {
    void *(*reserve)(size_t size);   
    bool (*commit)(void *addr, size_t total_size, size_t additional_bytes);    
    bool (*decommit)(void *addr, size_t size);  
    bool (*release)(void *addr, size_t size);  
    size_t page_size;               // System page size
} V_Allocator;

// MARK: WIN32
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>

    static void *v_alloc_win_reserve(size_t size);
    static bool v_alloc_win_commit(void *addr, size_t total_size, size_t additional_bytes);
    static bool v_alloc_win_decommit(void *addr, size_t size);
    static bool v_alloc_win_release(void *addr, size_t size);

    V_Allocator v_alloc = {
        .reserve = v_alloc_win_reserve,
        .commit = v_alloc_win_commit,
        .decommit = v_alloc_win_decommit,
        .release = v_alloc_win_release,
        .page_size = 0,
    };
    static size_t v_alloc_win_get_page_size(){
        SYSTEM_INFO sys_info;
        GetSystemInfo(&sys_info);
        return (size_t)sys_info.dwPageSize;
    }
    static void *v_alloc_win_reserve(size_t size) {
        if(v_alloc.page_size == 0){
            v_alloc.page_size = v_alloc_win_get_page_size();
        }
        return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    }
    static bool v_alloc_win_commit(void *addr, size_t total_size, size_t additional_bytes) {
        (void)additional_bytes;
        void *result = VirtualAlloc(addr, total_size, MEM_COMMIT, PAGE_READWRITE);
        return result ? true : false;
    }
    static bool v_alloc_win_decommit(void *addr, size_t extra_size) {
        // VirtualFree(base_addr + 1MB, MEM_DECOMMIT, extra_size);
    /* 
        "The VirtualFree function can decommit a range of pages that are in 
        different states, some committed and some uncommitted. This means 
        that you can decommit a range of pages without first determining 
        the current commitment state of each page."
    */
        BOOL success = VirtualFree(addr, MEM_DECOMMIT, (DWORD)extra_size);
        return success ? true : false;
    }
    static bool v_alloc_win_release(void *addr, size_t size) {
        (void)size; 
        return VirtualFree(addr, 0, MEM_RELEASE);
    }

   // MARK: LINUX
#elif defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
    #include <sys/mman.h>

    static void *v_alloc_posix_reserve(size_t size);
    static bool v_alloc_posix_commit(void *addr, size_t total_size, size_t additional_bytes);
    static bool v_alloc_posix_decommit(void *addr, size_t extra_size);
    static bool v_alloc_posix_release(void *addr, size_t size);

    V_Allocator v_alloc = {
        .reserve = v_alloc_posix_reserve,
        .commit = v_alloc_posix_commit,
        .decommit = v_alloc_posix_decommit,
        .release = v_alloc_posix_release,
        .page_size = 0,
    };
    static size_t v_alloc_posix_get_page_size(){
        s32 page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            //todo: error ?
            return 0;
        }
        return (size_t)page_size;
    }
    static void *v_alloc_posix_reserve(size_t size) {
        if (v_alloc.page_size == 0) {
            v_alloc.page_size = v_alloc_posix_get_page_size();
        }
        void *ptr = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        return ptr == MAP_FAILED ? NULL : ptr;
    }
    static bool v_alloc_posix_commit(void *addr, size_t total_size, size_t additional_bytes) {
        addr = (char *)addr + total_size - additional_bytes;
        s32 result = mprotect(addr, additional_bytes, PROT_READ | PROT_WRITE);
        return result ? true : false;
    }
    static bool v_alloc_posix_decommit(void *addr, size_t extra_size) {
        s32 result = madvise(addr, extra_size, MADV_DONTNEED);
        if(result == 0){
            result = mprotect(addr, extra_size, PROT_NONE);
        }
        return result == 0;
    }
    static bool v_alloc_posix_release(void *addr, size_t size) {
        return munmap(addr, size) == 0 ? true : false;
    }
#else
    #error "Unsupported platform"
#endif
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: v_alloc
// /////////////////////////////////////////////
// /////////////////////////////////////////////

// returns true on success, false on fail
bool v_alloc_reserve(AllocInfo *alloc_info, size_t reserve_size) {
    alloc_info->base = (char*)v_alloc.reserve(reserve_size);
    if (alloc_info->base == NULL) {
        return false; // initialization failed
    }
    alloc_info->ptr = alloc_info->base;
    alloc_info->end = alloc_info->base; // because we're only reserving
    alloc_info->reserved_size = reserve_size;
    alloc_info->page_size = v_alloc.page_size;
    return true; 
}
// commits initial size or grows alloc_info by additional size, returns NULL on fail
void* v_alloc_committ(AllocInfo *alloc_info, size_t additional_bytes) {
    if(additional_bytes == 0){ // we will consider this an error
        return NULL;
    }
    additional_bytes = ALIGN_UP(additional_bytes, DATA_ALIGNMENT);
    if (additional_bytes > (size_t)(alloc_info->end - alloc_info->ptr)) {
        if(alloc_info->base == 0){ // reserve default
            if(!v_alloc_reserve(alloc_info, MAX_ARENA_CAPACITY)){
                return NULL; // unable to reserve memory
            }
        }
        // internally we align_up to page_size
        size_t adjusted_additional_bytes = ALIGN_UP(additional_bytes, alloc_info->page_size);

        if (alloc_info->ptr + adjusted_additional_bytes > alloc_info->base + alloc_info->reserved_size) {
            return NULL; // out of reserved memory
        }
        size_t new_size = alloc_info->end - alloc_info->base + adjusted_additional_bytes;
        s32 result = v_alloc.commit(alloc_info->base, new_size, adjusted_additional_bytes);
        if (result == -1) {
            return NULL; // failed commit
        }
        alloc_info->end = alloc_info->base + new_size;
    }
    void* ptr = alloc_info->ptr;
    alloc_info->ptr = alloc_info->ptr + additional_bytes; 
    return ptr; 
}
void v_alloc_reset(AllocInfo *alloc_info) {
    // reset the pointer to the start of the committed region
    // todo: decommit
    if(alloc_info){
        alloc_info->ptr = alloc_info->base;
    }
}
bool v_alloc_decommit(AllocInfo *alloc_info, size_t extra_size) {
    if (!alloc_info || extra_size == 0) {
        return false; // invalid input
    }
    // ensure extra_size is aligned to the page size
    extra_size = ALIGN_UP(extra_size, alloc_info->page_size);
    // ensure extra_size does not exceed the committed memory size
    if (extra_size > (size_t)(alloc_info->end - alloc_info->base)) {
        return false; // cannot decommit more memory than is committed
    }
    // ensure the decommit region is page-aligned
    char *decommit_start = ALIGN_DOWN_PTR(alloc_info->end - extra_size, alloc_info->page_size);
    // decommit the memory
    bool result = v_alloc.decommit(decommit_start, extra_size);
    if (result) {
        alloc_info->end = decommit_start;
    }
    return result;
}
bool v_alloc_free(AllocInfo* alloc_info) {
    if (alloc_info->base == NULL) {
        return false; // nothing to free
    }
    return v_alloc.release(alloc_info->base, alloc_info->reserved_size);
}


// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DARR
// /////////////////////////////////////////////
// /////////////////////////////////////////////

#define DARR_MAX_CAPACITY ((size_t)UINT32_MAX - 2)

void darr__free(void *arr){ 
    DarrHdr *a = darr__hdr(arr);
    switch (a->alloc_type) 
    {
        case ALLOC_VIRTUAL:{
            AllocInfo *temp = a->alloc_info;
            v_alloc_free(a->alloc_info);
            free(temp);
            break;
        }
        case ALLOC_MALLOC:{
            free(a);
            break;
        }
    }
}
// optionally specify an initial capacity and/or an allocation type. Default (ALLOC_MALLOC or 0) uses malloc/realloc
// ALLOC_VIRTUAL uses a reserve/commit strategy, virtualalloc on win32, w/ stable pointers
void *darr__init(void *arr, size_t initial_capacity, size_t elem_size, AllocType alloc_type){
    if(arr) {
        dmap_error_handler("darr_init: array already initialized (argument must be null)");
    }
    DarrHdr *new_hdr = NULL;
    size_t new_cap = MAX(DARR_INITIAL_CAPACITY, initial_capacity); 
    size_t size_in_bytes = offsetof(DarrHdr, data) + (new_cap * elem_size);
    if(new_cap > DARR_MAX_CAPACITY){
        dmap_error_handler("Error: Max size exceeded\n");
    }
    if(size_in_bytes > DARR_DEFAULT_MAX_SIZE){
        dmap_error_handler("Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overied default.");
    }
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
        {
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                dmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            AllocInfo *alloc_info = (AllocInfo*)malloc(sizeof(AllocInfo));
            if(!alloc_info){
                dmap_error_handler("Allocation failed 0");
            }
            memset(alloc_info, 0, sizeof(AllocInfo));
            if(!v_alloc_committ(alloc_info, size_in_bytes)) {
                dmap_error_handler("Allocation failed 1");
            }
            new_hdr = (DarrHdr*)(alloc_info->base);
            new_hdr->alloc_info = alloc_info; 
            assert((size_t)(alloc_info->ptr - alloc_info->base) == offsetof(DarrHdr, data) + (new_cap * elem_size));
            break;
        }
        case ALLOC_MALLOC:
        {
            new_hdr = (DarrHdr*)malloc(size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 4");
            }
            new_hdr->alloc_info = NULL;
            break;
        }
    }
    new_hdr->alloc_type = alloc_type;
    new_hdr->len = 0;
    new_hdr->cap = (u32)new_cap;

    return new_hdr->data;
}
void *darr__grow(void *arr, size_t elem_size) { 
    if (!arr) {
        // when this is the case we just want the defaults
        return darr__init(arr, 0, elem_size, ALLOC_MALLOC);
    }
    DarrHdr *dh = darr__hdr(arr);
    DarrHdr *new_hdr = NULL;
    size_t old_cap = dh->cap;
    size_t total_size_in_bytes = offsetof(DarrHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER);
    if((size_t)(float)old_cap * DARR_GROWTH_MULTIPLIER > DARR_MAX_CAPACITY){
        dmap_error_handler("Error: Max size exceeded\n");
    }
    if(total_size_in_bytes > DARR_DEFAULT_MAX_SIZE){
        dmap_error_handler("Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overied default.");
    }
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            size_t additional_bytes = (size_t)((float)old_cap * (DARR_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            AllocInfo alloc_info = *dh->alloc_info;
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                dmap_error_handler("Allocation failed 2");
            }
            new_hdr = (DarrHdr*)alloc_info.base;
            *new_hdr->alloc_info = alloc_info;
            break;
        }
        case ALLOC_MALLOC: 
        {
            new_hdr = realloc(dh, total_size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 5");
            }
            break;
        }
    }
    new_hdr->cap = (u32)((float)old_cap * DARR_GROWTH_MULTIPLIER);
    assert(((size_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // Ensure alignment
    return &new_hdr->data;
}
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

struct DmapTable {
    u64 hash;
    union {
        u64 key;
        u64 rehash;
    };
    u32 data_idx;
};

#define DMAP_EMPTY   UINT32_MAX
#define DMAP_DELETED (UINT32_MAX - 1)
#define DMAP_MAX_CAPACITY ((size_t)UINT32_MAX - 2)


// declare hash functions
static inline u64 rapidhash_internal(const void *key, size_t len, u64 seed, const u64 *secret);

static const u64 RAPIDHASH_SECRET[3] = {
    0x9E3779B97F4A7C15ULL,  
    0xD6E8FEB86659FD93ULL,  
    0xCA9B0C7EBA1DA115ULL   
};

static u64 dmap_generate_hash(void *key, size_t key_size, u64 seed) {
    return rapidhash_internal(key, key_size, seed, RAPIDHASH_SECRET);
}

static bool keys_match(DmapTable *table, size_t idx, void *key, size_t key_size, KeyType key_type) {
    if(key_type == DMAP_U64){
        return memcmp(key, &table[idx].key, key_size) == 0;
    }
    else if(key_type == DMAP_STR){
        return dmap_generate_hash(key, key_size, table[idx].hash) == table[idx].rehash;
    }
    else {
        dmap_error_handler("invalid key type");
    }
    return false;
}
// return current or empty - for inserts - we can overwrite on insert
static size_t dmap_find_slot(void *dmap, void *key, size_t key_size, u64 hash){
    DmapHdr *d = dmap__hdr(dmap);
    DmapTable *table = d->table;
    // size_t idx = hash % hash_cap;
    // size_t idx = (hash ^ (hash >> 16)) % d->hash_cap;
    u32 idx = hash & (d->hash_cap - 1);
    size_t j = d->hash_cap;
    u32 result = DMAP_EMPTY;
    while(true){
        assert(j-- != 0); // unreachable - suggests there were no empty slots
        if(d->table[idx].data_idx == DMAP_EMPTY){
            result = idx;
            break;
        }
        if(d->table[idx].data_idx != DMAP_DELETED && d->table[idx].hash == hash){
            if(keys_match(table, idx, key, key_size, d->key_type)){
                result = idx;
                break;
            }
        }
        idx = (idx + 1) & (d->hash_cap - 1);
    }
    return result;
}
// grows the entry array of the hashmap to accommodate more elements
static void dmap_grow_table(void *dmap, size_t new_hash_cap, size_t old_hash_cap) {
    DmapHdr *d = dmap__hdr(dmap); // retrieve the hashmap header
    size_t new_size_in_bytes = new_hash_cap * sizeof(DmapTable);
    DmapTable *new_table = malloc(new_size_in_bytes);
    if (!new_table) {
        dmap_error_handler("Out of memory 1");
    }
    memset(new_table, 0xff, new_size_in_bytes); // set data indices to DMAP_EMPTY
    // if the hashmap has existing table, rehash them into the new entry array
    if (dmap_count(dmap)) {
        for (size_t i = 0; i < old_hash_cap; i++) {
            if(d->table[i].data_idx == DMAP_EMPTY) continue;
            // size_t idx = (d->table[i].hash ^ (d->table[i].hash >> 16)) % new_hash_cap;
            size_t idx = d->table[i].hash & (new_hash_cap - 1);
            size_t j = new_hash_cap;
            while(true){
                assert(j-- != 0); // unreachable, suggests no empty slot was found
                if(new_table[idx].data_idx == DMAP_EMPTY){
                    new_table[idx] = d->table[i];
                    break;
                }
                idx = (idx + 1) & (new_hash_cap - 1);
            }
        }
    }
    // replace the old entry array with the new one
    free(d->table);
    d->table = new_table;
}
static void *dmap__grow_internal(void *dmap, size_t elem_size) {
    if (!dmap) {
        dmap_error_handler("dmap not initialized; unreachable");
    }
    DmapHdr *dh = dmap__hdr(dmap);
    DmapHdr *new_hdr = NULL;
    size_t old_hash_cap = dh->hash_cap;
    size_t old_cap = dh->cap;
    size_t new_hash_cap = old_hash_cap * 2;
    size_t new_cap = (size_t)((float)new_hash_cap * DMAP_LOAD_FACTOR);
    size_t total_size_in_bytes = offsetof(DmapHdr, data) + (new_cap * elem_size);

    if (new_cap > DMAP_MAX_CAPACITY) {
        dmap_error_handler("Error: Max size exceeded\n");
    }
    if(total_size_in_bytes > DMAP_DEFAULT_MAX_SIZE){
        dmap_error_handler("Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overied default.");
    }
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            AllocInfo alloc_info = *dh->alloc_info;
            size_t additional_bytes = (new_cap - old_cap) * elem_size;
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                dmap_error_handler("Allocation failed 3");
            }
            new_hdr = (DmapHdr*)alloc_info.base;
            *new_hdr->alloc_info = alloc_info;
            break;
        }
        case ALLOC_MALLOC: 
        {
            new_hdr = realloc(dh, total_size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 2");
            }
            break;
        }
    }
    // grow the table to fit into the newly allocated space
    dmap_grow_table(new_hdr->data, new_hash_cap, old_hash_cap); 

    new_hdr->cap = (u32)new_cap;
    new_hdr->hash_cap = (u32)new_hash_cap;

    assert(((uintptr_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // ensure alignment
    return new_hdr->data; // return the aligned data pointer
}

static void *dmap__init_internal(void *dmap, size_t capacity, size_t elem_size, AllocType alloc_type, bool is_string){
    if(dmap) {
        dmap_error_handler("dmap_init: dmap already initialized, argument must be null");
    }
    DmapHdr *new_hdr = NULL;

    capacity = MAX((size_t)DMAP_INITIAL_CAPACITY, capacity);
    size_t table_capacity = next_power_of_2(capacity);
    while ((size_t)((float)table_capacity * DMAP_LOAD_FACTOR) < capacity) {
        if (table_capacity > SIZE_MAX / 2) {  // Prevent overflow
            dmap_error_handler("Error: exceeded max capacity");
        }
        table_capacity *= 2;
    }
    capacity = (size_t)((float)table_capacity * DMAP_LOAD_FACTOR);
    size_t size_in_bytes = offsetof(DmapHdr, data) + (capacity * elem_size);
    if(capacity > DMAP_MAX_CAPACITY){
        dmap_error_handler("Error: Max size exceeded\n");
    }
    if(size_in_bytes > DMAP_DEFAULT_MAX_SIZE){
        dmap_error_handler("Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overied default.");
    }
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
        {
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                dmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            AllocInfo *alloc_info = (AllocInfo*)malloc(sizeof(AllocInfo));
            if(!alloc_info){
                dmap_error_handler("Allocation failed 0");
            }
            memset(alloc_info, 0, sizeof(AllocInfo));
            if(!v_alloc_committ(alloc_info, size_in_bytes)) {
                dmap_error_handler("Allocation failed 4");
            }
            new_hdr = (DmapHdr*)alloc_info->base;
            new_hdr->alloc_info = alloc_info;
            break;
        }
        case ALLOC_MALLOC:
        {
            new_hdr = (DmapHdr*)malloc(size_in_bytes);
            if(!new_hdr){
                dmap_error_handler("Out of memory 3");
            }
            new_hdr->alloc_info = NULL;
            break;
        }
    }
    new_hdr->alloc_type = alloc_type;
    new_hdr->len = 0;
    new_hdr->cap = (u32)capacity;
    new_hdr->hash_cap = (u32)table_capacity;
    new_hdr->returned_idx = DMAP_EMPTY;
    new_hdr->table = NULL;
    new_hdr->free_list = NULL;
    new_hdr->key_type = DMAP_UNINITIALIZED;
    new_hdr->key_size = 0;
    new_hdr->hash_seed = dmap_generate_seed();
    new_hdr->key_type = is_string ? DMAP_STR : DMAP_U64;

    dmap_grow_table(new_hdr->data, new_hdr->hash_cap, 0);
    assert(((uintptr_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // ensure alignment
    return new_hdr->data;
}
void *dmap__kstr_init(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type){
    return dmap__init_internal(dmap, initial_capacity, elem_size, alloc_type, true);
}
void *dmap__init(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type){
    return dmap__init_internal(dmap, initial_capacity, elem_size, alloc_type, false);
}
// grows the hashmap to a new capacity
void *dmap__kstr_grow(void *dmap, size_t elem_size) {
    if (!dmap) {
        // when this is the case we just want the defaults
        AllocType alloc_type = ALLOC_MALLOC;
        return dmap__kstr_init(dmap, 0, elem_size, alloc_type);
    }
    return dmap__grow_internal(dmap, elem_size);
}
void *dmap__grow(void *dmap, size_t elem_size) {
    if (!dmap) {
        // when this is the case we just want the defaults
        AllocType alloc_type = ALLOC_MALLOC;
        return dmap__init(dmap, 0, elem_size, alloc_type);
    }
    return dmap__grow_internal(dmap, elem_size);
}
void dmap__free(void *dmap){
    DmapHdr *d = dmap__hdr(dmap);
    if(d){
        if(d->table) {
            free(d->table); 
        }
        if(d->free_list) {
            darr_free(d->free_list);
        }
        switch (d->alloc_type) 
        {
            case ALLOC_VIRTUAL:{
                AllocInfo *temp = d->alloc_info;
                v_alloc_free(d->alloc_info);
                free(temp);
                break;
            }
            case ALLOC_MALLOC:{
                free(dmap__hdr(dmap));
                break;
            }
        }
    }
}

static size_t dmap__get_entry_index(void *dmap, void *key, size_t key_size){
    size_t result = DMAP_INVALID;
    if(dmap_cap(dmap)!=0) {
        DmapHdr *d = dmap__hdr(dmap); // retrieve the header of the hashmap for internal structure access
        u64 hash = dmap_generate_hash(key, key_size, d->hash_seed); // generate a hash value for the given key
        size_t idx = hash & (d->hash_cap - 1);
        size_t j = d->hash_cap; // counter to ensure the loop doesn't iterate more than the capacity of the hashmap
        while(true) { // loop to search for the key in the hashmap
            assert(j-- != 0); // unreachable -- suggests table is full
            if(d->table[idx].data_idx == DMAP_EMPTY){ // if the entry is empty, the key is not in the hashmap
                break;
            }
            if(d->table[idx].data_idx != DMAP_DELETED && d->table[idx].hash == hash) {
                if(keys_match(d->table, idx, key, key_size, d->key_type)){
                    result = idx;
                    break;
                }
            }
            idx = (idx + 1) & (d->hash_cap - 1); // move to the next index, wrapping around to the start if necessary
        }
    }
    return result;
}
void dmap__insert_entry(void *dmap, void *key, size_t key_size, bool is_string){ 
    DmapHdr *d = dmap__hdr(dmap);
    if(d->key_type == DMAP_UNINITIALIZED){ // todo: should not go here
        d->key_type = is_string ? DMAP_STR : DMAP_U64;
    }
    if(d->key_size == 0){ 
        if(d->key_type == DMAP_STR) 
            d->key_size = UINT32_MAX; // strings
        else 
            d->key_size = (u32)key_size;
    }
    else if(d->key_size != key_size && d->key_size != UINT32_MAX){
        char err[256];
        snprintf(err, 256, "Key is not the correct type/size, it should be %u bytes, but is %zu bytes.\n", d->key_size, key_size);
        dmap_error_handler(err);
    }
    u64 hash = dmap_generate_hash(key, key_size, d->hash_seed);
    // todo: finish implementing the union idea - starting with the hash we already use.
    size_t idx = dmap_find_slot(dmap, key, key_size, hash);

    if(d->table[idx].data_idx != DMAP_EMPTY && d->table[idx].data_idx != DMAP_DELETED){
        d->returned_idx = d->table[idx].data_idx;
    }
    else {
        d->returned_idx = darr_len(d->free_list) ? darr_pop(d->free_list) : d->len;
        d->len += 1;

        DmapTable *entry = &d->table[idx];
        entry->hash = hash;
        entry->data_idx = d->returned_idx;
        switch (d->key_type) {
            case DMAP_U64: {
                entry->key = 0;  // zero-out first
                memcpy(&entry->key, key, key_size);
                break;
            }
            case DMAP_STR:{
                entry->rehash = dmap_generate_hash(key, key_size, hash);  // rehash
                break;
            }
            case DMAP_UNINITIALIZED:{
            default:
                dmap_error_handler("Invalid KeyType in insert_entry.\n");
            }
        }
    }
    return;
}
// returns: size_t - The index of the entry if the key is found, or DMAP_INVALID if the key is not present
bool dmap__find_data_idx(void *dmap, void *key, size_t key_size){
    if(!dmap){
        return false;
    }
    DmapHdr *d = dmap__hdr(dmap);
    if(d->key_size != key_size && d->key_size != UINT32_MAX){ 
        char err[128];
        snprintf(err, 128, "GET: Key is not the correct type, it should be %u bytes, but is %zu bytes.\n", d->key_size, key_size);
        dmap_error_handler(err);
    }
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_INVALID) { 
        return false; // entry is not found
    }
    d->returned_idx = d->table[idx].data_idx;
    return true;
}
// returns: size_t - The index of the data associated with the key, or DMAP_INVALID (UINT32_MAX) if the key is not found
size_t dmap__get_idx(void *dmap, void *key, size_t key_size){
    DmapHdr *d = dmap__hdr(dmap);
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    return d->table[idx].data_idx;
}
    // returns the data index of the deleted entry. Caller may wish to mark data as invalid
size_t dmap__delete(void *dmap, void *key, size_t key_size){
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    DmapHdr *d = dmap__hdr(dmap);
    u32 data_index = d->table[idx].data_idx;
    darr_push(d->free_list, data_index);
    d->table[idx].data_idx = DMAP_DELETED;
    d->len -= 1; 
    return data_index;
}
size_t dmap_kstr_delete(void *dmap, void *key, size_t key_size){
    return dmap__delete(dmap, key, key_size);
}
size_t dmap_kstr_get_idx(void *dmap, void *key, size_t key_size){
    return dmap__get_idx(dmap, key, key_size);
}
// len of the data array, including invalid table. For iterating
size_t dmap_range(void *dmap){ 
    return dmap ? dmap__hdr(dmap)->len + darr_len(dmap__hdr(dmap)->free_list) : 0; 
} 

// MARK: hash function:
// - rapidhash source repository: https://github.com/Nicoshev/rapidhash

/*
 * rapidhash - Very fast, high quality, platform-independent hashing algorithm.
 * Copyright (C) 2024 Nicolas De Carli
 *
 * Based on 'wyhash', by Wang Yi <godspeed_china@yeah.net>
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
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
 *
 * You can contact the author at:
 *   - rapidhash source repository: https://github.com/Nicoshev/rapidhash
 */

/*
 *  Includes.
 */
#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
  #include <intrin.h>
  #if defined(_M_X64) && !defined(_M_ARM64EC)
    #pragma intrinsic(_umul128)
  #endif
#endif

/*
 *  C++ macros.
 *
 *  RAPIDHASH_INLINE can be overridden to be stronger than a hint, i.e. by adding __attribute__((always_inline)).
 */
#ifdef __cplusplus
  #define RAPIDHASH_NOEXCEPT noexcept
  #define RAPIDHASH_CONSTEXPR constexpr
  #ifndef RAPIDHASH_INLINE
    #define RAPIDHASH_INLINE inline
  #endif
#else
  #define RAPIDHASH_NOEXCEPT
  #define RAPIDHASH_CONSTEXPR static const
  #ifndef RAPIDHASH_INLINE
    #define RAPIDHASH_INLINE static inline
  #endif
#endif

/*
 *  Protection macro, alters behaviour of rapid_mum multiplication function.
 *
 *  RAPIDHASH_FAST: Normal behavior, max speed.
 *  RAPIDHASH_PROTECTED: Extra protection against entropy loss.
 */
#ifndef RAPIDHASH_PROTECTED
  #define RAPIDHASH_FAST
#elif defined(RAPIDHASH_FAST)
  #error "cannot define RAPIDHASH_PROTECTED and RAPIDHASH_FAST simultaneously."
#endif

/*
 *  Unrolling macros, changes code definition for main hash function.
 *
 *  RAPIDHASH_COMPACT: Legacy variant, each loop process 48 bytes.
 *  RAPIDHASH_UNROLLED: Unrolled variant, each loop process 96 bytes.
 *
 *  Most modern CPUs should benefit from having RAPIDHASH_UNROLLED.
 *
 *  These macros do not alter the output hash.
 */
#ifndef RAPIDHASH_COMPACT
  #define RAPIDHASH_UNROLLED
#elif defined(RAPIDHASH_UNROLLED)
  #error "cannot define RAPIDHASH_COMPACT and RAPIDHASH_UNROLLED simultaneously."
#endif

/*
 *  Likely and unlikely macros.
 */
#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
  #define _likely_(x)  __builtin_expect(x,1)
  #define _unlikely_(x)  __builtin_expect(x,0)
#else
  #define _likely_(x) (x)
  #define _unlikely_(x) (x)
#endif

/*
 *  Endianness macros.
 */
#ifndef RAPIDHASH_LITTLE_ENDIAN
  #if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #define RAPIDHASH_LITTLE_ENDIAN
  #elif defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define RAPIDHASH_BIG_ENDIAN
  #else
    #warning "could not determine endianness! Falling back to little endian."
    #define RAPIDHASH_LITTLE_ENDIAN
  #endif
#endif

/*
 *  Default seed.
 */
#define RAPID_SEED (0xbdd89aa982704029ull)

/*
 *  Default secret parameters.
 */
RAPIDHASH_CONSTEXPR uint64_t rapid_secret[3] = {0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull};

/*
 *  64*64 -> 128bit multiply function.
 *
 *  @param A  Address of 64-bit number.
 *  @param B  Address of 64-bit number.
 *
 *  Calculates 128-bit C = *A * *B.
 *
 *  When RAPIDHASH_FAST is defined:
 *  Overwrites A contents with C's low 64 bits.
 *  Overwrites B contents with C's high 64 bits.
 *
 *  When RAPIDHASH_PROTECTED is defined:
 *  Xors and overwrites A contents with C's low 64 bits.
 *  Xors and overwrites B contents with C's high 64 bits.
 */
RAPIDHASH_INLINE void rapid_mum(uint64_t *A, uint64_t *B) RAPIDHASH_NOEXCEPT {
#if defined(__SIZEOF_INT128__)
  __uint128_t r=*A; r*=*B;
  #ifdef RAPIDHASH_PROTECTED
  *A^=(uint64_t)r; *B^=(uint64_t)(r>>64);
  #else
  *A=(uint64_t)r; *B=(uint64_t)(r>>64);
  #endif
#elif defined(_MSC_VER) && (defined(_WIN64) || defined(_M_HYBRID_CHPE_ARM64))
  #if defined(_M_X64)
    #ifdef RAPIDHASH_PROTECTED
    uint64_t a, b;
    a=_umul128(*A,*B,&b);
    *A^=a;  *B^=b;
    #else
    *A=_umul128(*A,*B,B);
    #endif
  #else
    #ifdef RAPIDHASH_PROTECTED
    uint64_t a, b;
    b = __umulh(*A, *B);
    a = *A * *B;
    *A^=a;  *B^=b;
    #else
    uint64_t c = __umulh(*A, *B);
    *A = *A * *B;
    *B = c;
    #endif
  #endif
#else
  uint64_t ha=*A>>32, hb=*B>>32, la=(uint32_t)*A, lb=(uint32_t)*B, hi, lo;
  uint64_t rh=ha*hb, rm0=ha*lb, rm1=hb*la, rl=la*lb, t=rl+(rm0<<32), c=t<rl;
  lo=t+(rm1<<32); c+=lo<t; hi=rh+(rm0>>32)+(rm1>>32)+c;
  #ifdef RAPIDHASH_PROTECTED
  *A^=lo;  *B^=hi;
  #else
  *A=lo;  *B=hi;
  #endif
#endif
}

/*
 *  Multiply and xor mix function.
 *
 *  @param A  64-bit number.
 *  @param B  64-bit number.
 *
 *  Calculates 128-bit C = A * B.
 *  Returns 64-bit xor between high and low 64 bits of C.
 */
RAPIDHASH_INLINE uint64_t rapid_mix(uint64_t A, uint64_t B) RAPIDHASH_NOEXCEPT { rapid_mum(&A,&B); return A^B; }

/*
 *  Read functions.
 */
#ifdef RAPIDHASH_LITTLE_ENDIAN
RAPIDHASH_INLINE uint64_t rapid_read64(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint64_t v; memcpy(&v, p, sizeof(uint64_t)); return v;}
RAPIDHASH_INLINE uint64_t rapid_read32(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint32_t v; memcpy(&v, p, sizeof(uint32_t)); return v;}
#elif defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
RAPIDHASH_INLINE uint64_t rapid_read64(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint64_t v; memcpy(&v, p, sizeof(uint64_t)); return __builtin_bswap64(v);}
RAPIDHASH_INLINE uint64_t rapid_read32(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint32_t v; memcpy(&v, p, sizeof(uint32_t)); return __builtin_bswap32(v);}
#elif defined(_MSC_VER)
RAPIDHASH_INLINE uint64_t rapid_read64(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint64_t v; memcpy(&v, p, sizeof(uint64_t)); return _byteswap_uint64(v);}
RAPIDHASH_INLINE uint64_t rapid_read32(const uint8_t *p) RAPIDHASH_NOEXCEPT { uint32_t v; memcpy(&v, p, sizeof(uint32_t)); return _byteswap_ulong(v);}
#else
RAPIDHASH_INLINE uint64_t rapid_read64(const uint8_t *p) RAPIDHASH_NOEXCEPT {
  uint64_t v; memcpy(&v, p, 8);
  return (((v >> 56) & 0xff)| ((v >> 40) & 0xff00)| ((v >> 24) & 0xff0000)| ((v >>  8) & 0xff000000)| ((v <<  8) & 0xff00000000)| ((v << 24) & 0xff0000000000)| ((v << 40) & 0xff000000000000)| ((v << 56) & 0xff00000000000000));
}
RAPIDHASH_INLINE uint64_t rapid_read32(const uint8_t *p) RAPIDHASH_NOEXCEPT {
  uint32_t v; memcpy(&v, p, 4);
  return (((v >> 24) & 0xff)| ((v >>  8) & 0xff00)| ((v <<  8) & 0xff0000)| ((v << 24) & 0xff000000));
}
#endif

/*
 *  Reads and combines 3 bytes of input.
 *
 *  @param p  Buffer to read from.
 *  @param k  Length of @p, in bytes.
 *
 *  Always reads and combines 3 bytes from memory.
 *  Guarantees to read each buffer position at least once.
 *
 *  Returns a 64-bit value containing all three bytes read.
 */
RAPIDHASH_INLINE uint64_t rapid_readSmall(const uint8_t *p, size_t k) RAPIDHASH_NOEXCEPT { return (((uint64_t)p[0])<<56)|(((uint64_t)p[k>>1])<<32)|p[k-1];}

/*
 *  rapidhash main function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *  @param secret  Triplet of 64-bit secrets used to alter hash result predictably.
 *
 *  Returns a 64-bit hash.
 */
RAPIDHASH_INLINE uint64_t rapidhash_internal(const void *key, size_t len, uint64_t seed, const uint64_t* secret) RAPIDHASH_NOEXCEPT {
  const uint8_t *p=(const uint8_t *)key; seed^=rapid_mix(seed^secret[0],secret[1])^len;  uint64_t  a,  b;
  if(_likely_(len<=16)){
    if(_likely_(len>=4)){
      const uint8_t * plast = p + len - 4;
      a = (rapid_read32(p) << 32) | rapid_read32(plast);
      const uint64_t delta = ((len&24)>>(len>>3));
      b = ((rapid_read32(p + delta) << 32) | rapid_read32(plast - delta)); }
    else if(_likely_(len>0)){ a=rapid_readSmall(p,len); b=0;}
    else a=b=0;
  }
  else{
    size_t i=len;
    if(_unlikely_(i>48)){
      uint64_t see1=seed, see2=seed;
#ifdef RAPIDHASH_UNROLLED
      while(_likely_(i>=96)){
        seed=rapid_mix(rapid_read64(p)^secret[0],rapid_read64(p+8)^seed);
        see1=rapid_mix(rapid_read64(p+16)^secret[1],rapid_read64(p+24)^see1);
        see2=rapid_mix(rapid_read64(p+32)^secret[2],rapid_read64(p+40)^see2);
        seed=rapid_mix(rapid_read64(p+48)^secret[0],rapid_read64(p+56)^seed);
        see1=rapid_mix(rapid_read64(p+64)^secret[1],rapid_read64(p+72)^see1);
        see2=rapid_mix(rapid_read64(p+80)^secret[2],rapid_read64(p+88)^see2);
        p+=96; i-=96;
      }
      if(_unlikely_(i>=48)){
        seed=rapid_mix(rapid_read64(p)^secret[0],rapid_read64(p+8)^seed);
        see1=rapid_mix(rapid_read64(p+16)^secret[1],rapid_read64(p+24)^see1);
        see2=rapid_mix(rapid_read64(p+32)^secret[2],rapid_read64(p+40)^see2);
        p+=48; i-=48;
      }
#else
      do {
        seed=rapid_mix(rapid_read64(p)^secret[0],rapid_read64(p+8)^seed);
        see1=rapid_mix(rapid_read64(p+16)^secret[1],rapid_read64(p+24)^see1);
        see2=rapid_mix(rapid_read64(p+32)^secret[2],rapid_read64(p+40)^see2);
        p+=48; i-=48;
      } while (_likely_(i>=48));
#endif
      seed^=see1^see2;
    }
    if(i>16){
      seed=rapid_mix(rapid_read64(p)^secret[2],rapid_read64(p+8)^seed^secret[1]);
      if(i>32)
        seed=rapid_mix(rapid_read64(p+16)^secret[2],rapid_read64(p+24)^seed);
    }
    a=rapid_read64(p+i-16);  b=rapid_read64(p+i-8);
  }
  a^=secret[1]; b^=seed;  rapid_mum(&a,&b);
  return  rapid_mix(a^secret[0]^len,b^secret[1]);
}

/*
 *  rapidhash default seeded hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *
 *  Calls rapidhash_internal using provided parameters and default secrets.
 *
 *  Returns a 64-bit hash.
 */
RAPIDHASH_INLINE uint64_t rapidhash_withSeed(const void *key, size_t len, uint64_t seed) RAPIDHASH_NOEXCEPT {
  return rapidhash_internal(key, len, seed, rapid_secret);
}

/*
 *  rapidhash default hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *
 *  Calls rapidhash_withSeed using provided parameters and the default seed.
 *
 *  Returns a 64-bit hash.
 */
RAPIDHASH_INLINE uint64_t rapidhash(const void *key, size_t len) RAPIDHASH_NOEXCEPT {
  return rapidhash_withSeed(key, len, RAPID_SEED);
}

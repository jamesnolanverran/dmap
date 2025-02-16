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
    #include <assert.h>
#endif

// #if defined(_MSC_VER) || defined(_WIN32)
//     #include <intrin.h>  // MSVC intrinsics
// #endif

// static inline size_t next_power_of_2(size_t x) {
//     if (x <= 1) return 1;  // Ensure minimum value of 1

// #if defined(_MSC_VER) || defined(_WIN32)
//     unsigned long index;
//     if (_BitScanReverse64(&index, x - 1)) {
//         return 1ULL << (index + 1);
//     }
// #else
//     return 1ULL << (64 - __builtin_clzl(x - 1));
// #endif

//     return 1;
// }

#include <time.h>
#if defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
#endif
#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#endif

uint64_t dmap_generate_seed() {
    uint64_t seed = 14695981039346656037ULL; // FNV-1a offset basis
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


#define DMAP_EMPTY (UINT32_MAX)
#define DMAP_DELETED (UINT32_MAX - 1)

#ifndef MIN
#define MIN(x, y) ((x) <= (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) >= (y) ? (x) : (y))
#endif

#define ALIGN_DOWN(n, a) ((n) & ~((a) - 1))
#define ALIGN_UP(n, a) ALIGN_DOWN((n) + (a) - 1, (a))

#define ALIGN_DOWN_PTR(p, a) ((void *)ALIGN_DOWN((uintptr_t)(p), (a)))
#define ALIGN_UP_PTR(p, a) ((void *)ALIGN_UP((uintptr_t)(p), (a)))

typedef signed char        s8; 
typedef short              s16;
typedef int                s32;
typedef long long          s64;
typedef unsigned char      u8; 
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

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
    if(size_in_bytes > UINT32_MAX - 2){
        dmap_error_handler("Error: Max size exceeded\n");
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
    size_t old_cap = darr_cap(arr);
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            size_t additional_bytes = (size_t)((float)old_cap * (DARR_GROWTH_MULTIPLIER - 1.0f) * elem_size);

            size_t total_size_in_bytes = offsetof(DarrHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER);
            size_t max_capacity = (size_t)UINT32_MAX - 2;
            if(total_size_in_bytes > max_capacity){
                dmap_error_handler("Error: Max size exceeded\n");
            }
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
            size_t new_size_in_bytes = offsetof(DarrHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER);
            size_t max_capacity = (size_t)UINT32_MAX - 2;
            if(new_size_in_bytes > max_capacity){
                dmap_error_handler("Error: Max size exceeded\n");
            }
            new_hdr = realloc(dh, new_size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 5");
            }
            break;
        }
    }
    new_hdr->cap += (u32)old_cap;
    assert(((size_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // Ensure alignment
    return &new_hdr->data;
}
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

struct DmapEntry {
    u64 hash;
    union {
        u64 key;
        u64 rehash;
    };
    u32 data_idx;
};

// declare hash function
static u64 dmap_fnv_64(void *buf, size_t len, u64 hval);

static u64 dmap_generate_hash(void *key, size_t key_size, u64 seed) {
    return dmap_fnv_64(key, key_size, seed);
}

static bool keys_match(DmapEntry *entries, size_t idx, u64 hash, void *key, size_t key_size, KeyType key_type) {
    bool result = false;
    DmapEntry *entry = &entries[idx];
    if(entry->hash == hash) { // don't need a switch here or enums - todo
        switch (key_type) {
            case DMAP_U64:
                if(memcmp(key, &entry->key, key_size) == 0) result = true;
                break;
            case DMAP_STR: {
                u64 rehash = dmap_fnv_64(key, key_size, entry->hash);
                if(entry->rehash == rehash) result = true;
                break;
            }
            case DMAP_UNINITIALIZED: {
                dmap_error_handler("invalid key type");
                break;
            }
        }
    }
    return result;
}
// return current or empty - for inserts - we can overwrite on insert
static size_t dmap_find_slot(DmapEntry *entries, void *key, size_t key_size, u64 hash, size_t hash_cap, KeyType key_type){
    // size_t idx = hash % hash_cap;
    size_t idx = (hash ^ (hash >> 16)) % hash_cap;
    // size_t idx = hash & (hash_cap - 1);
    size_t j = hash_cap;
    while(true){
        if(j-- == 0) assert(false); // unreachable - suggests there were no empty slots
        if(entries[idx].data_idx == DMAP_EMPTY || entries[idx].data_idx == DMAP_DELETED){
            return idx;
        }
        if(keys_match(entries, idx, hash, key, key_size, key_type)){
            return idx;
        }
        idx += 1;
        if(idx >= hash_cap){
            idx = 0;
        }
    }
}
// grows the entry array of the hashmap to accommodate more elements
static void dmap_grow_entries(void *dmap, size_t new_hash_cap, size_t old_hash_cap) {
    DmapHdr *d = dmap__hdr(dmap); // retrieve the hashmap header
    size_t new_size_in_bytes = new_hash_cap * sizeof(DmapEntry);
    DmapEntry *new_entries = (DmapEntry*)malloc(new_size_in_bytes);
    if (!new_entries) {
        dmap_error_handler("Out of memory 1");
    }
    memset(new_entries, 0xff, new_size_in_bytes);

    // if the hashmap has existing entries, rehash them into the new entry array
    if (dmap_count(dmap)) {
        for (size_t i = 0; i < old_hash_cap; i++) {
            if(d->entries[i].data_idx == DMAP_EMPTY || d->entries[i].data_idx == DMAP_DELETED) continue;
            size_t idx = (d->entries[i].hash ^ (d->entries[i].hash >> 16)) % new_hash_cap;
            size_t j = new_hash_cap;
            while(true){
                if(j-- == 0) assert(false); // unreachable - suggests there were no empty slots
                if(new_entries[idx].data_idx == DMAP_EMPTY){
                    new_entries[idx] = d->entries[i];
                    break;
                }
                idx += 1;
                if(idx >= new_hash_cap){
                    idx = 0;
                }
            }
        }
    }
    // replace the old entry array with the new one
    free(d->entries);
    d->entries = new_entries;
}
static void *dmap__grow_internal(void *dmap, size_t elem_size) {
    if (!dmap) {
        dmap_error_handler("dmap not initialized; unreachable");
    }
    DmapHdr *dh = dmap__hdr(dmap);
    
    DmapHdr *new_hdr = NULL;
    size_t old_cap = dmap_cap(dmap);
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            AllocInfo alloc_info = *dh->alloc_info;
            size_t additional_bytes = (size_t)((float)old_cap * (DMAP_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            size_t total_size_in_bytes = offsetof(DmapHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER);
            size_t max_capacity = (size_t)UINT32_MAX - 2;
            if(total_size_in_bytes > max_capacity){
                dmap_error_handler("Error: Max size exceeded\n");
            }
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                dmap_error_handler("Allocation failed 3");
            }
            new_hdr = (DmapHdr*)alloc_info.base;
            *new_hdr->alloc_info = alloc_info;
            break;
        }
        case ALLOC_MALLOC: 
        {
            size_t new_size_in_bytes = offsetof(DmapHdr, data) + (size_t)((float)old_cap * (float)elem_size * DMAP_GROWTH_MULTIPLIER); 
            size_t max_capacity = (size_t)UINT32_MAX - 2;
            if(new_size_in_bytes > max_capacity){
                dmap_error_handler("Error: Max size exceeded\n");
            }
            new_hdr = realloc(dh, new_size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 2");
            }
            break;
        }
    }
    new_hdr->cap = (size_t)((float)old_cap * DMAP_GROWTH_MULTIPLIER);

    size_t new_hash_cap = (size_t)((float)new_hdr->cap * DMAP_HASHTABLE_MULTIPLIER); 
    // new_hash_cap = next_power_of_2(new_hash_cap);

    size_t old_hash_cap = new_hdr->hash_cap;

    new_hdr->hash_cap = (u32)new_hash_cap;
    // grow the entries to fit into the newly allocated space
    dmap_grow_entries(new_hdr->data, new_hash_cap, old_hash_cap); 

    assert(((uintptr_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // ensure alignment
    return new_hdr->data; // return the aligned data pointer
}

static void *dmap__init_internal(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type, bool is_string){
    if(dmap) {
        dmap_error_handler("dmap_init: dmap already initialized, argument must be null");
    }
    DmapHdr *new_hdr = NULL;
    initial_capacity = MAX(DMAP_INITIAL_CAPACITY, initial_capacity); 
    size_t size_in_bytes = offsetof(DmapHdr, data) + (initial_capacity * elem_size);
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
    new_hdr->cap = (u32)initial_capacity;
    new_hdr->hash_cap = (size_t)((float)initial_capacity * DMAP_HASHTABLE_MULTIPLIER);
    new_hdr->returned_idx = DMAP_EMPTY;
    new_hdr->entries = NULL;
    new_hdr->free_list = NULL;
    new_hdr->key_type = DMAP_UNINITIALIZED;
    new_hdr->key_size = 0;
    new_hdr->hash_seed = dmap_generate_seed();

    if(is_string){
        new_hdr->key_type = DMAP_STR;
    }
    else {
        new_hdr->key_type = DMAP_UNINITIALIZED;
    }
    dmap_grow_entries(new_hdr->data, new_hdr->hash_cap, 0);
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
        if(d->entries) {
            free(d->entries); 
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
// clear/reset the dmap without freeing memory
void dmap_clear(void *dmap){
    if(!dmap) {
        dmap_error_handler("dmap_clear: argument must not be null");
    }
    DmapHdr *d = dmap__hdr(dmap);
    for (size_t i = 0; i < d->hash_cap; i++) { // entries are empty by default
        d->entries[i].data_idx = UINT32_MAX;
    }
    darr_clear(d->free_list);
    d->len = 0;
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
    // get a fresh data slot
    d->returned_idx = darr_len(d->free_list) ? darr_pop(d->free_list) : d->len;
    d->len += 1;
    u64 hash = dmap_generate_hash(key, key_size, d->hash_seed);
    // todo: finish implementing the union idea - starting with the hash we already use.
    size_t idx = dmap_find_slot(d->entries, key, key_size, hash, d->hash_cap, d->key_type);
    assert(idx != DMAP_INVALID);
    DmapEntry *entry = &d->entries[idx];
    switch (d->key_type) {
        case DMAP_U64: {
            entry->hash = hash;
            entry->data_idx = d->returned_idx;
            entry->key = 0;  // zero-out first
            memcpy(&entry->key, key, key_size);
            break;
        }
        case DMAP_STR:{
            entry->hash = hash;
            entry->data_idx = d->returned_idx;
            entry->rehash = dmap_fnv_64(key, key_size, hash);  // rehash
            break;
        }
        case DMAP_UNINITIALIZED:{
        default:
            dmap_error_handler("Invalid KeyType in insert_entry.\n");
        }
    }
}
// returns: size_t - The index of the entry if the key is found, or DMAP_INVALID if the key is not present
static size_t dmap__get_entry_index(void *dmap, void *key, size_t key_size){
    if(dmap_cap(dmap)==0) {
        return DMAP_INVALID; // check if the hashmap is empty or NULL
    }
    DmapHdr *d = dmap__hdr(dmap); // retrieve the header of the hashmap for internal structure access
    u64 hash = dmap_generate_hash(key, key_size, d->hash_seed); // generate a hash value for the given key
    // size_t idx = hash % d->hash_cap; // calculate the initial index to start the search in the hash table
    size_t idx = (hash ^ (hash >> 16)) % d->hash_cap;
    // size_t idx = hash & (d->hash_cap - 1);
    size_t j = d->hash_cap; // counter to ensure the loop doesn't iterate more than the capacity of the hashmap
    // size_t total_probes = 0;

    while(true) { // loop to search for the key in the hashmap
        if(j-- == 0) assert(false); // unreachable -- suggests entries is full
        if(d->entries[idx].data_idx == DMAP_EMPTY){ // if the entry is empty, the key is not in the hashmap
            return DMAP_INVALID;
        }
        if(d->entries[idx].data_idx != DMAP_DELETED && keys_match(d->entries, idx, hash, key, key_size, d->key_type)) {
            // if(total_probes > 3){
                // printf("Total probes: %zu\n", total_probes);
            // }
            return idx;
        }
        // total_probes++;
        
        idx += 1; // move to the next index, wrapping around to the start if necessary
        if(idx >= d->hash_cap) { 
            idx = 0; 
        } 
    }
}
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
    d->returned_idx = d->entries[idx].data_idx;
    return true;
}
// returns: size_t - The index of the data associated with the key, or DMAP_INVALID (UINT32_MAX) if the key is not found
size_t dmap__get_idx(void *dmap, void *key, size_t key_size){
    DmapHdr *d = dmap__hdr(dmap);
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    return d->entries[idx].data_idx;
}
    // returns the data index of the deleted entry. Caller may wish to mark data as invalid
size_t dmap__delete(void *dmap, void *key, size_t key_size){
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    DmapHdr *d = dmap__hdr(dmap);
    u32 data_index = d->entries[idx].data_idx;
    d->entries[idx].data_idx = UINT32_MAX - 1;
    darr_push(d->free_list, data_index);
    d->len -= 1; 
    return data_index;
}
size_t dmap_kstr_delete(void *dmap, void *key, size_t key_size){
    return dmap__delete(dmap, key, key_size);
}
size_t dmap_kstr_get_idx(void *dmap, void *key, size_t key_size){
    return dmap__get_idx(dmap, key, key_size);
}
// len of the data array, including invalid entries. For iterating
size_t dmap_range(void *dmap){ 
    return dmap ? dmap__hdr(dmap)->len + darr_len(dmap__hdr(dmap)->free_list) : 0; 
} 

// hash function:
static u64 dmap_fnv_64(void *buf, size_t len, u64 hval) { // fnv_64a unsigned char *bp = (unsigned char *)buf;
    unsigned char *bp = (unsigned char *)buf;
    unsigned char *be = bp + len;
    while (bp < be) {
        hval ^= (u64)*bp++;
        hval *= 0x100000001b3ULL;
        // hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40);
    }
    return hval;
}

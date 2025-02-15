#include "dmap.h"
#include <stdlib.h> 
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

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
    #define assert(cond) ((void)0)
#endif

#define DMAP_EMPTY (SIZE_MAX)
#define DMAP_DELETED (SIZE_MAX - 1)

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
        case ALLOC_VIRTUAL:
            v_alloc_free(&a->alloc_info);
            break;
        case ALLOC_MALLOC:
            free(a);
            break;
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
    AllocInfo alloc_info = {0};
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                dmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            if(!v_alloc_committ(&alloc_info, size_in_bytes)) {
                dmap_error_handler("Allocation failed");
            }
            new_hdr = (DarrHdr*)alloc_info.base;
            assert((size_t)(alloc_info.ptr - alloc_info.base) == offsetof(DarrHdr, data) + (new_cap * elem_size));
            break;
        case ALLOC_MALLOC:
            new_hdr = (DarrHdr*)malloc(size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 4");
            }
            break;
    }
    new_hdr->alloc_info = alloc_info; // copy values
    new_hdr->alloc_type = alloc_type;
    new_hdr->len = 0;
    new_hdr->cap = new_cap;

    return new_hdr->data;
}
void *darr__grow(void *arr, size_t elem_size) { 
    if (!arr) {
        // when this is the case we just want the defaults
        return darr__init(arr, 0, elem_size, ALLOC_MALLOC);
    }
    DarrHdr *dh = darr__hdr(arr);
    DarrHdr *new = NULL;
    AllocInfo alloc_info = dh->alloc_info;
    size_t old_cap = darr_cap(arr);
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            size_t additional_bytes = (size_t)((float)old_cap * (DARR_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                dmap_error_handler("Allocation failed");
            }
            new = (DarrHdr*)alloc_info.base;
            break;
        }
        case ALLOC_MALLOC: 
        {
            size_t new_size_in_bytes = offsetof(DarrHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER); 
            new = realloc(dh, new_size_in_bytes);
            if(!new) {
                dmap_error_handler("Out of memory 5");
            }
            break;
        }
    }
    new->alloc_info = alloc_info;
    new->cap += old_cap;
    assert(((size_t)&new->data & (DATA_ALIGNMENT - 1)) == 0); // Ensure alignment
    return &new->data;
}
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

typedef struct {
    u64 hash;
    u32 key;
    u32 data_idx;
} DmapEntry_U32;

typedef struct {
    u64 hash;
    u64 key;
    u64 data_idx;
} DmapEntry_U64;

// string and > 8 bytes
typedef struct {
    u64 hash;
    u64 rehash;
    u64 data_idx;
} DmapEntry_STR;

// declare hash function
static u64 dmap_fnv_64(void *buf, size_t len, u64 hval);

static u64 dmap_generate_hash(void *key, size_t key_size, u64 seed) {
    return dmap_fnv_64(key, key_size, seed);
}
// return a void* to entry
static inline void* get_entry(void *entries, size_t idx, KeyType key_type) {
    switch (key_type) {
        case DMAP_U32: return &((DmapEntry_U32 *)entries)[idx];
        case DMAP_U64: return &((DmapEntry_U64 *)entries)[idx];
        case DMAP_STR: return &((DmapEntry_STR *)entries)[idx];
        case DMAP_UNINITIALIZED:
        default:       dmap_error_handler("Invalid KeyType in entry_data_idx.");
    }
    return NULL;
}
static size_t get_data_index(void *entries, size_t idx, KeyType key_type) {
    void *entry = get_entry(entries, idx, key_type);
    switch (key_type) {
        case DMAP_U32: return ((DmapEntry_U32*)entry)->data_idx;
        case DMAP_U64: return ((DmapEntry_U64*)entry)->data_idx;
        case DMAP_STR: return ((DmapEntry_STR*)entry)->data_idx;
        case DMAP_UNINITIALIZED:
        default:
            dmap_error_handler("Invalid KeyType in get_data_index.");
            return UINT64_MAX;  
    }
}
static void mark_index_empty(void *entries, size_t entry_index, KeyType key_type) {
    void *entry = get_entry(entries, entry_index, key_type);
    switch (key_type) {
        case DMAP_U32: ((DmapEntry_U32*)entry)->data_idx = UINT32_MAX; break;
        case DMAP_U64: ((DmapEntry_U64*)entry)->data_idx = UINT64_MAX; break;
        case DMAP_STR: ((DmapEntry_STR*)entry)->data_idx = UINT64_MAX; break;
        case DMAP_UNINITIALIZED:
        default:
            dmap_error_handler("Invalid KeyType in mark_index_empty.");
    }
}
static void mark_index_deleted(void *entries, size_t entry_index, KeyType key_type) {
    void *entry = get_entry(entries, entry_index, key_type);
    switch (key_type) {
        case DMAP_U32: ((DmapEntry_U32*)entry)->data_idx = UINT32_MAX - 1; break;
        case DMAP_U64: ((DmapEntry_U64*)entry)->data_idx = UINT64_MAX - 1; break;
        case DMAP_STR: ((DmapEntry_STR*)entry)->data_idx = UINT64_MAX - 1; break;
        case DMAP_UNINITIALIZED:
        default:
            dmap_error_handler("Invalid KeyType in mark_index_deleted.");
    }
}
static bool entry_is_empty(void *entries, size_t i, KeyType key_type) {
    void *entry = get_entry(entries, i, key_type);
    switch (key_type) {
        case DMAP_U32: return ((DmapEntry_U32*)entry)->data_idx == UINT32_MAX;
        case DMAP_U64: return ((DmapEntry_U64*)entry)->data_idx == UINT64_MAX;
        case DMAP_STR: return ((DmapEntry_STR*)entry)->data_idx == UINT64_MAX;
        case DMAP_UNINITIALIZED:
        default:
            return false;
    }
}
static bool entry_is_deleted(void *entries, size_t i, KeyType key_type) {
    void *entry = get_entry(entries, i, key_type);
    switch (key_type) {
        case DMAP_U32: return ((DmapEntry_U32*)entry)->data_idx == (UINT32_MAX - 1);
        case DMAP_U64: return ((DmapEntry_U64*)entry)->data_idx == (UINT64_MAX - 1);
        case DMAP_STR: return ((DmapEntry_STR*)entry)->data_idx == (UINT64_MAX - 1);
        case DMAP_UNINITIALIZED:
        default:
            return false;
    }
}
static bool entry_is_empty_or_deleted(void *entries, size_t i, KeyType key_type) {
    void *entry = get_entry(entries, i, key_type);
    switch (key_type) {
        case DMAP_U32: return ((DmapEntry_U32*)entry)->data_idx == UINT32_MAX || ((DmapEntry_U32*)entry)->data_idx == (UINT32_MAX - 1);
        case DMAP_U64: return ((DmapEntry_U64*)entry)->data_idx == UINT64_MAX || ((DmapEntry_U64*)entry)->data_idx == (UINT64_MAX - 1);
        case DMAP_STR: return ((DmapEntry_STR*)entry)->data_idx == UINT64_MAX || ((DmapEntry_STR*)entry)->data_idx == (UINT64_MAX - 1);
        case DMAP_UNINITIALIZED:
        default:
            return false;
    }
}
static void store_entry_data(void *entries, size_t entry_index, void *key, size_t key_size, u64 hash, size_t data_index, KeyType key_type) {
    void *entry = get_entry(entries, entry_index, key_type);

    // Store data index and hash
    switch (key_type) {
        case DMAP_U32:
            ((DmapEntry_U32*)entry)->hash = hash;
            ((DmapEntry_U32*)entry)->data_idx = (u32)data_index;
            memcpy(&((DmapEntry_U32*)entry)->key, key, sizeof(u32));
            break;
        case DMAP_U64:
            ((DmapEntry_U64*)entry)->hash = hash;
            ((DmapEntry_U64*)entry)->data_idx = (u64)data_index;
            memcpy(&((DmapEntry_U64*)entry)->key, key, sizeof(u64));
            break;
        case DMAP_STR:
            ((DmapEntry_STR*)entry)->data_idx = data_index;
            ((DmapEntry_STR*)entry)->hash = hash;
            ((DmapEntry_STR*)entry)->rehash = dmap_fnv_64(key, key_size, hash);
            break;
        case DMAP_UNINITIALIZED:
        default:
            dmap_error_handler("Invalid KeyType in store_entry_data.");
    }
}
static bool keys_match(void *entries, size_t idx, u64 hash, void *key, size_t key_size, KeyType key_type) {
    void *entry = get_entry(entries, idx, key_type);
    
    u64 stored_hash = *(u64 *)entry;
    if (stored_hash != hash) return false;

    switch (key_type) {
        case DMAP_U32:
            return memcmp(key, &((DmapEntry_U32*)entry)->key, sizeof(u32)) == 0;
        case DMAP_U64:
            return memcmp(key, &((DmapEntry_U64*)entry)->key, sizeof(u64)) == 0;
        case DMAP_STR: {
            DmapEntry_STR *str_entry = (DmapEntry_STR*)entry;
            u64 rehash = dmap_fnv_64(key, key_size, str_entry->hash);
            return str_entry->rehash == rehash;  // Hashes already matched, so only rehash check is needed
        }
        case DMAP_UNINITIALIZED:
        default:
            return false;
    }
}

static void *allocate_entries(size_t num_entries, size_t dmap_cap, KeyType key_type, size_t *out_size, KeyType *new_key_type) {
    size_t entry_size = 0;
    if (dmap_cap <= UINT32_MAX - 2) {
        *new_key_type = (key_type == DMAP_STR) ? DMAP_STR : DMAP_U32;
    } 
    else {
        *new_key_type = (key_type == DMAP_STR) ? DMAP_STR : DMAP_U64;
    }
    switch (*new_key_type) {
        case DMAP_U32: entry_size = sizeof(DmapEntry_U32); break;
        case DMAP_U64: entry_size = sizeof(DmapEntry_U64); break;
        case DMAP_STR: entry_size = sizeof(DmapEntry_STR); break;
        case DMAP_UNINITIALIZED:
        default:
            *out_size = 0;
            return NULL;
    }
    *out_size = num_entries * entry_size;  
    return malloc(*out_size);
}
static size_t dmap_find_empty_slot(void *entries, u64 hash, size_t hash_cap, KeyType type){
    size_t idx = hash % hash_cap;
    size_t j = hash_cap;
    while(true){
        if(j-- == 0) assert(false); // unreachable - suggests there were no empty slots
        if(entry_is_empty_or_deleted(entries, idx, type)){
            return idx;
        }
        idx += 1;
        if(idx >= hash_cap){
            idx = 0;
        }
    }
}
static size_t dmap_find_slot(void *entries, void *key, size_t key_size, u64 hash, size_t hash_cap, KeyType type){
    size_t idx = hash % hash_cap;
    size_t j = hash_cap;
    while(true){
        if(j-- == 0) assert(false); // unreachable - suggests there were no empty slots
        if(entry_is_empty_or_deleted(entries, idx, type)){
            return idx;
        }
        if(keys_match(entries, idx, hash, key, key_size, type)){
            return idx;
        }
        idx += 1;
        if(idx >= hash_cap){
            idx = 0;
        }
    }
}
static void set_new_index(void *new_entries, void *old_entries, size_t idx, size_t new_hash_cap, KeyType new_type, KeyType old_type) {
    size_t new_index;
    void *old_entry = get_entry(old_entries, idx, old_type);
    void *new_entry = NULL;

    // since hash is always first, we can access it directly
    u64 hash = *(u64 *)old_entry;  

    new_index = dmap_find_empty_slot(new_entries, hash, new_hash_cap, new_type);
    
    new_entry = get_entry(new_entries, new_index, new_type);

    // handle struct size changes correctly
    if (old_type == DMAP_U32 && new_type == DMAP_U64) {
        DmapEntry_U32 *old_u32 = (DmapEntry_U32 *)old_entry;
        DmapEntry_U64 *new_u64 = (DmapEntry_U64 *)new_entry;

        new_u64->hash = old_u32->hash;
        new_u64->key = old_u32->key;   
        new_u64->data_idx = old_u32->data_idx;
    } else {
        size_t entry_size = (new_type == DMAP_U32) ? sizeof(DmapEntry_U32) :
                            (new_type == DMAP_U64) ? sizeof(DmapEntry_U64) :
                            sizeof(DmapEntry_STR);  
        memcpy(new_entry, old_entry, entry_size);
    }
}
static KeyType determine_initial_config(size_t key_size, size_t initial_capacity) {
    // Determine key size (assume <=8 bytes are stored directly)
    if (key_size == 1 || key_size == 2 || key_size == 4) { // u8, u16, u32
        if (initial_capacity <= UINT32_MAX - 2) return DMAP_U32; 
        return DMAP_U64; // if capacity is very large, make everything 64bit
    } 
    else if (key_size == 8) {
        return DMAP_U64;
    } 
    else {
        // strings, structs, etc.
        return DMAP_STR;
    }
}
// grows the entry array of the hashmap to accommodate more elements
static void dmap_grow_entries(void *dmap, size_t new_hash_cap, size_t old_hash_cap) {
    DmapHdr *d = dmap__hdr(dmap); // retrieve the hashmap header
    size_t new_size_in_bytes;
    KeyType new_key_type;
    void *new_entries = allocate_entries(new_hash_cap, d->cap, d->key_type, &new_size_in_bytes, &new_key_type); // allocate new memory for the entries
    // allocate should also set the NEW_Dmap_config as well, right?
    if (!new_entries) {
        dmap_error_handler("Out of memory 1");
    }
    memset(new_entries, 0xff, new_size_in_bytes);

    // if the hashmap has existing entries, rehash them into the new entry array
    if (dmap_count(dmap)) {
        for (size_t i = 0; i < old_hash_cap; i++) {
            if(entry_is_empty_or_deleted(d->entries, i, d->key_type)) continue; // skip empty entries
            // find a new empty slot for the entry and update its position
            set_new_index(new_entries, d->entries, i, new_hash_cap, new_key_type, d->key_type); // need NEW key_type as well, right?
        }
    }
    d->key_type = new_key_type;
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
    AllocInfo alloc_info = dh->alloc_info;
    size_t old_cap = dmap_cap(dmap);
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: 
        {
            size_t additional_bytes = (size_t)((float)old_cap * (DMAP_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                dmap_error_handler("Allocation failed");
            }
            new_hdr = (DmapHdr*)alloc_info.base;
            break;
        }
        case ALLOC_MALLOC: 
        {
            size_t new_size_in_bytes = offsetof(DmapHdr, data) + (size_t)((float)old_cap * (float)elem_size * DMAP_GROWTH_MULTIPLIER); // double capacity
            new_hdr = realloc(dh, new_size_in_bytes);
            if(!new_hdr) {
                dmap_error_handler("Out of memory 2");
            }
            break;
        }
    }
    new_hdr->alloc_info = alloc_info;
    new_hdr->cap = (size_t)((float)old_cap * DMAP_GROWTH_MULTIPLIER);

    size_t new_hash_cap = (size_t)((float)new_hdr->cap * DMAP_HASHTABLE_MULTIPLIER); 
    size_t old_hash_cap = new_hdr->hash_cap;

    new_hdr->hash_cap = new_hash_cap;
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
    AllocInfo alloc_info = {0};
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                dmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            if(!v_alloc_committ(&alloc_info, size_in_bytes)) {
                dmap_error_handler("Allocation failed");
            }
            new_hdr = (DmapHdr*)alloc_info.base;
        break;
        case ALLOC_MALLOC:
            new_hdr = (DmapHdr*)malloc(size_in_bytes);
            if(!new_hdr){
                dmap_error_handler("Out of memory 3");
            }
        break;
    }
    new_hdr->alloc_info = alloc_info;
    new_hdr->alloc_type = alloc_type;
    new_hdr->len = 0;
    new_hdr->cap = initial_capacity;
    new_hdr->hash_cap = (size_t)((float)initial_capacity * DMAP_HASHTABLE_MULTIPLIER);
    new_hdr->returned_idx = DMAP_EMPTY;
    new_hdr->is_string = is_string;
    new_hdr->entries = NULL;
    new_hdr->free_list = NULL;
    new_hdr->key_type = DMAP_UNINITIALIZED;
    new_hdr->key_size = 0;

    if(new_hdr->is_string){
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
            case ALLOC_VIRTUAL:
                v_alloc_free(&d->alloc_info);
                break;
            case ALLOC_MALLOC:
                free(dmap__hdr(dmap));
                break;
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
        mark_index_empty(d->entries, i, d->key_type);
    }
    darr_clear(d->free_list);
    d->len = 0;
}
void dmap__insert_entry(void *dmap, void *key, size_t key_size){ 
    DmapHdr *d = dmap__hdr(dmap);
    if(d->key_type == DMAP_UNINITIALIZED){
        d->key_type = determine_initial_config(key_size, d->cap);
    }
    if(d->key_size == 0){ 
        if(d->key_type == DMAP_STR) 
            d->key_size = UINT64_MAX; // strings
        else 
            d->key_size = key_size;
    }
    else if(d->key_size != key_size && d->key_size != UINT64_MAX){
        dmap_error_handler("Key is not the correct type");
    }
    // get a fresh data slot
    d->returned_idx = darr_len(d->free_list) ? darr_pop(d->free_list) : d->len;
    d->len += 1;
    u64 hash = dmap_generate_hash(key, key_size, 1333);
    // todo: finish implementing the union idea - starting with the hash we already use.
    size_t entry_index = dmap_find_slot(d->entries, key, key_size, hash, d->hash_cap, d->key_type);

    store_entry_data(d->entries, entry_index, key, key_size, hash, d->returned_idx, d->key_type);
}
// returns: size_t - The index of the entry if the key is found, or DMAP_EMPTY if the key is not present
static size_t dmap__get_entry_index(void *dmap, void *key, size_t key_size){
    if(dmap_cap(dmap)==0) {
        return DMAP_EMPTY; // check if the hashmap is empty or NULL
    }
    DmapHdr *d = dmap__hdr(dmap); // retrieve the header of the hashmap for internal structure access
    u64 hash = dmap_generate_hash(key, key_size, 1333); // generate a hash value for the given key
    size_t idx = hash % d->hash_cap; // calculate the initial index to start the search in the hash table
    size_t j = d->hash_cap; // counter to ensure the loop doesn't iterate more than the capacity of the hashmap

    while(true) { // loop to search for the key in the hashmap
        if(j-- == 0) assert(false); // unreachable -- suggests entries is full
        if(entry_is_empty(d->entries, idx, d->key_type)) {  // if the entry is empty, the key is not in the hashmap
            return DMAP_EMPTY;
        }
        if(!entry_is_deleted(d->entries, idx, d->key_type) && keys_match(d->entries, idx, hash, key, key_size, d->key_type)) {
            return idx;
        }
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
    if(d->key_size != UINT64_MAX && d->key_size != key_size){
        dmap_error_handler("GET: Key is not the correct type");
    }
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_EMPTY) { 
        return false; // entry is not found
    }
    d->returned_idx = get_data_index(d->entries, idx, d->key_type);
    return true;
}
// returns: size_t - The index of the data associated with the key, or DMAP_EMPTY (SIZE_MAX) if the key is not found
size_t dmap__get_idx(void *dmap, void *key, size_t key_size){
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_EMPTY) {
        return (u64)DMAP_EMPTY;
    }
    DmapHdr *d = dmap__hdr(dmap);
    return get_data_index(d->entries, idx, d->key_type);
}
    // returns the data index of the deleted entry. Caller may wish to mark data as invalid
size_t dmap__delete(void *dmap, void *key, size_t key_size){
    size_t idx = dmap__get_entry_index(dmap, key, key_size);
    if(idx == DMAP_EMPTY) {
        return DMAP_EMPTY;
    }
    DmapHdr *d = dmap__hdr(dmap);
    size_t data_index = get_data_index(d->entries, idx, d->key_type);
    mark_index_deleted(d->entries, idx, d->key_type);
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
static u64 dmap_fnv_64(void *buf, size_t len, u64 hval) { // fnv_64a
    unsigned char *bp = (unsigned char *)buf;
    unsigned char *be = bp + len;
    while (bp < be) {
        hval ^= (u64)*bp++;
        hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40);
    }
    return hval;
}


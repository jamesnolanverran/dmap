#include "hmap.h"
#include <stdlib.h> 

#ifdef HMAP_DEBUG
    #include <stdio.h>
#endif

#ifdef HMAP_DEBUG
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
static void hmap_default_error_handler(char* err_msg) {
    perror(err_msg);
    exit(1); 
}
static void (*hmap_error_handler)(char* err_msg) = hmap_default_error_handler;

void hmap_set_error_handler(void (*handler)(char* err_msg)) {
    hmap_error_handler = handler ? handler : hmap_default_error_handler; // fallback to default
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
        (void)size; // Suppress unused parameter warning
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
        return false; // Initialization failed
    }
    alloc_info->ptr = alloc_info->base;
    alloc_info->end = alloc_info->base; // because we're only reserving
    alloc_info->reserved_size = reserve_size;
    alloc_info->page_size = v_alloc.page_size;
    return true; 
}
// commits initial size or grows alloc_info by additional size, returns NULL on fail
void* v_alloc_committ(AllocInfo *alloc_info, size_t additional_bytes) {
    if(additional_bytes == 0){ // consider this an error
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
    // Reset the pointer to the start of the committed region
    if(alloc_info){
        alloc_info->ptr = alloc_info->base;
    }
}
bool v_alloc_decommit(AllocInfo *alloc_info, size_t extra_size) {
    if (!alloc_info || extra_size == 0) {
        return false; // Invalid input
    }
    // Ensure extra_size is aligned to the page size
    extra_size = ALIGN_UP(extra_size, alloc_info->page_size);
    // Ensure extra_size does not exceed the committed memory size
    if (extra_size > (size_t)(alloc_info->end - alloc_info->base)) {
        return false; // Cannot decommit more memory than is committed
    }
    // Ensure the decommit region is page-aligned
    char *decommit_start = ALIGN_DOWN_PTR(alloc_info->end - extra_size, alloc_info->page_size);
    // Decommit the memory
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
        hmap_error_handler("darr_init: array already initialized, argument must be null");
    }
    DarrHdr *new_hdr = NULL;
    size_t new_cap = MAX(DARR_INITIAL_CAPACITY, initial_capacity); 
    size_t size_in_bytes = offsetof(DarrHdr, data) + (new_cap * elem_size);
    AllocInfo alloc_info = {0};
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                hmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            if(!v_alloc_committ(&alloc_info, size_in_bytes)) {
                hmap_error_handler("Allocation failed");
            }
            new_hdr = (DarrHdr*)alloc_info.base;
            assert((size_t)(alloc_info.ptr - alloc_info.base) == offsetof(DarrHdr, data) + (new_cap * elem_size));
            break;
        case ALLOC_MALLOC:
            new_hdr = (DarrHdr*)malloc(size_in_bytes);
            if(!new_hdr) {
                hmap_error_handler("Out of memory");
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
        case ALLOC_VIRTUAL: {
            size_t additional_bytes = (size_t)((float)old_cap * (DARR_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                hmap_error_handler("Allocation failed");
            }
            new = (DarrHdr*)alloc_info.base;
            break;
        }
        case ALLOC_MALLOC: {
            size_t new_size_in_bytes = offsetof(DarrHdr, data) + (size_t)((float)old_cap * (float)elem_size * DARR_GROWTH_MULTIPLIER); // double capacity
            new = realloc(dh, new_size_in_bytes);
            if(!new) {
                hmap_error_handler("Out of memory");
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
// MARK: HMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

// declare hash function
void MurmurHash3_x64_128(const void *key, s32 len, u32 seed, void *out);

static void hmap_generate_hash(void *key, size_t key_size, u64 hash_out[2]) {
    MurmurHash3_x64_128(key, (s32)key_size, 0x9747b28c, hash_out);
}
static size_t hmap_find_empty_slot(HmapEntry *entries, u64 hash[2], size_t hash_cap){
    size_t idx = (hash[0] ^ hash[1]) % hash_cap;
    size_t j = hash_cap;
    while(true){
        if(j-- == 0) assert(false); // unreachable - suggests there were no empty slots
        if(entries[idx].data_index == HMAP_EMPTY || entries[idx].data_index == HMAP_DELETED){
            return idx;
        }
        if(entries[idx].hash[0] == hash[0] && entries[idx].hash[1] == hash[1]){
            return HMAP_ALREADY_EXISTS;
        }
        idx += 1;
        if(idx >= hash_cap){
            idx = 0;
        }
    }
}
// Grows the entry array of the hashmap to accommodate more elements
static void hmap_grow_entries(void *hmap, size_t new_hash_cap, size_t elem_size) {
    HMapHdr *d = hmap__hdr(hmap); // Retrieve the hashmap header
    size_t new_size_in_bytes = new_hash_cap * elem_size; // Calculate the new size in bytes for the entries
    size_t old_hash_cap = d->hash_cap;
    HmapEntry *new_entries = malloc(new_size_in_bytes); // Allocate new memory for the entries
    if (!new_entries) {
        hmap_error_handler("Out of memory");
    }
    memset(new_entries, 0xff, new_size_in_bytes); // Initialize all bits to 1 (used for HMAP_EMPTY marker)
    // If the hashmap has existing entries, rehash them into the new entry array
    if (hmap_count(hmap)) {
        for (size_t i = 0; i < old_hash_cap; i++) {
            if(d->entries[i].data_index == HMAP_EMPTY) continue; // Skip empty entries
            if(d->entries[i].data_index == HMAP_DELETED) continue; // Skip deleted entries
            // Find a new empty slot for the entry and update its position
            size_t new_index = hmap_find_empty_slot(new_entries, d->entries[i].hash, new_hash_cap);
            new_entries[new_index] = d->entries[i];
        }
    }
    // Replace the old entry array with the new one
    if (d->entries) {
        free(d->entries);
    }
    d->entries = new_entries;
}
// Grows the hashmap to a new capacity
void *hmap__grow(void *hmap, size_t elem_size) {
    if (!hmap) {
        // when this is the case we just want the defaults
        AllocType alloc_type = ALLOC_VIRTUAL;
        #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
            alloc_type = ALLOC_MALLOC;
        #endif
        return hmap__init(hmap, 0, elem_size, alloc_type);
    }
    HMapHdr *new_hdr = NULL;
    HMapHdr *dh = hmap__hdr(hmap);
    AllocInfo alloc_info = dh->alloc_info;
    size_t old_cap = hmap_cap(hmap);
    switch (dh->alloc_type) 
    {
        case ALLOC_VIRTUAL: {
            size_t additional_bytes = (size_t)((float)old_cap * (HMAP_GROWTH_MULTIPLIER - 1.0f) * elem_size);
            if(!v_alloc_committ(&alloc_info, additional_bytes)) {
                hmap_error_handler("Allocation failed");
            }
            new_hdr = (HMapHdr*)alloc_info.base;
            break;
        }
        case ALLOC_MALLOC: {
            size_t new_size_in_bytes = offsetof(HMapHdr, data) + (size_t)((float)old_cap * (float)elem_size * HMAP_GROWTH_MULTIPLIER); // double capacity
            new_hdr = realloc(dh, new_size_in_bytes);
            if(!new_hdr) {
                hmap_error_handler("Out of memory");
            }
            break;
        }
    }
    new_hdr->alloc_info = alloc_info;
    new_hdr->cap = (size_t)((float)old_cap * HMAP_GROWTH_MULTIPLIER);
    size_t new_hash_cap = (size_t)((float)new_hdr->cap * HMAP_HASHTABLE_MULTIPLIER);
    // Grow the entries to fit into the newly allocated space
    hmap_grow_entries(new_hdr->data, new_hash_cap, sizeof(HmapEntry)); // entries should be double the capacity
    new_hdr->hash_cap = new_hash_cap;

    assert(((uintptr_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // Ensure alignment
    return new_hdr->data; // Return the aligned data pointer
}
void *hmap__init(void *hmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type){
    if(hmap) {
        hmap_error_handler("hmap_init: hmap already initialized, argument must be null");
    }
    HMapHdr *new_hdr = NULL;
    initial_capacity = MAX(HMAP_INITIAL_CAPACITY, initial_capacity); 
    size_t size_in_bytes = offsetof(HMapHdr, data) + (initial_capacity * elem_size);
    AllocInfo alloc_info = {0};
    switch (alloc_type) 
    {
        case ALLOC_VIRTUAL:
            #if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
                hmap_error_handler("ALLOC_VIRTUAL not supported on this platform; use ALLOC_MALLOC");
            #endif
            if(!v_alloc_committ(&alloc_info, size_in_bytes)) {
                hmap_error_handler("Allocation failed");
            }
            new_hdr = (HMapHdr*)alloc_info.base;
        break;
        case ALLOC_MALLOC:
            new_hdr = (HMapHdr*)malloc(size_in_bytes);
            if(!new_hdr){
                hmap_error_handler("Out of memory");
            }
        break;
    }
    new_hdr->alloc_info = alloc_info;
    new_hdr->alloc_type = alloc_type;
    new_hdr->len = 0;
    new_hdr->cap = initial_capacity;
    new_hdr->hash_cap = (size_t)((float)initial_capacity * HMAP_HASHTABLE_MULTIPLIER);
    new_hdr->returned_idx = HMAP_EMPTY;
    new_hdr->entries = NULL;
    new_hdr->free_list = NULL;
    hmap_grow_entries(new_hdr->data, new_hdr->hash_cap, sizeof(HmapEntry));
    assert(((uintptr_t)&new_hdr->data & (DATA_ALIGNMENT - 1)) == 0); // Ensure alignment
    return new_hdr->data;
}
void hmap__free(void *hmap){
    HMapHdr *d = hmap__hdr(hmap);
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
                free(hmap__hdr(hmap));
                break;
        }
    }
}
// clear/reset the hmap without freeing memory
void hmap_clear(void *hmap){
    if(!hmap) {
        hmap_error_handler("hmap_clear: argument must not be null");
    }
    HMapHdr *d = hmap__hdr(hmap);
    memset(d->entries, 0xff, d->hash_cap * sizeof(HmapEntry)); // Initialize all bits to 1 (used for HMAP_EMPTY marker)
    darr_clear(d->free_list);
    d->len = 0;
}
bool hmap__insert_entry(void *hmap, void *key, size_t key_size){ 
    HMapHdr *d = hmap__hdr(hmap);
    // get a fresh data slot
    d->returned_idx = darr_len(d->free_list) ? darr_pop(d->free_list) : d->len;
    d->len += 1;
    u64 hash_out[2];
    hmap_generate_hash(key, key_size, hash_out);
    size_t entry_index = hmap_find_empty_slot(d->entries, hash_out, d->hash_cap);
    if(entry_index == HMAP_ALREADY_EXISTS){
        return false;
    }
    d->entries[entry_index] = (HmapEntry){d->returned_idx, {hash_out[0], hash_out[1]}};  
    // we use a free list to keep track of empty slots in the data array from deletions. Use those first. 
    return true;
}
// Function: hmap__get_entry_index
// Description: Searches for a key in the hashmap and returns its index if found.
//              This function is used internally for operations like insertions or deletions.
// Parameters:
//   void *hmap - Pointer to the hashmap in which to search for the key.
//   void *key - Pointer to the key to be searched.
//   size_t key_size - Size of the key.
// Returns:
//   size_t - The index of the entry where the key is found, or HMAP_EMPTY if the key is not present.
static size_t hmap__get_entry_index(void *hmap, void *key, size_t key_size){
    if(hmap_cap(hmap)==0) {
        return HMAP_EMPTY; // Check if the hashmap is empty or NULL
    }
    HMapHdr *d = hmap__hdr(hmap); // Retrieve the header of the hashmap for internal structure access.
    u64 hash[2];
    hmap_generate_hash(key, key_size, hash); // Generate a hash value for the given key.
    size_t idx = (hash[0] ^ hash[1]) % d->hash_cap; // Calculate the initial index to start the search in the hash table.
    size_t j = d->hash_cap; // Counter to ensure the loop doesn't iterate more than the capacity of the hashmap.

    while(true) { // Loop to search for the key in the hashmap.
        if(j-- == 0) assert(false); // unreachable -- suggests entries is full
        if(d->entries[idx].data_index == HMAP_EMPTY) {  // if the entry is empty, the key is not in the hashmap.
            return HMAP_EMPTY;
        }
        if(d->entries[idx].hash[0] == hash[0] && d->entries[idx].hash[1] == hash[1]) { // If the hash matches, the correct entry has been found.
            return idx;
        }
        idx += 1; // Move to the next index, wrapping around to the start if necessary.
        if(idx >= d->hash_cap) { 
            idx = 0; 
        } 
    }
}
bool hmap__find_data_idx(void *hmap, void *key, size_t key_size){
    size_t idx = hmap__get_entry_index(hmap, key, key_size);
    if(idx == HMAP_EMPTY) { 
        return false; // entry is not found
    }
    HMapHdr *d = hmap__hdr(hmap);
    d->returned_idx = d->entries[idx].data_index;
    return true;
}
// Function: hmap_get
// Description: Retrieves the index of the data associated with a given key in a hashmap.
// Parameters:
//   void *hmap - Pointer to the hashmap from which the data index is to be retrieved.
//   void *key - Pointer to the key for which the data index is required.
//   size_t key_size - Size of the key.
// Returns:
//   size_t - The index of the data associated with the key, or NOT_FOUND (SIZE_MAX) if the key is not found.
size_t hmap__get_idx(void *hmap, void *key, size_t key_size){
    size_t idx = hmap__get_entry_index(hmap, key, key_size);
    if(idx == HMAP_EMPTY) {
        return HMAP_EMPTY;
    }
    HMapHdr *d = hmap__hdr(hmap);
    return d->entries[idx].data_index;
}
size_t hmap__delete(void *hmap, void *key, size_t key_size){
    size_t idx = hmap__get_entry_index(hmap, key, key_size);
    if(idx == HMAP_EMPTY) {
        return HMAP_EMPTY;
    }
    HMapHdr *d = hmap__hdr(hmap);
    size_t data_index = d->entries[idx].data_index;
    d->entries[idx].data_index = HMAP_DELETED;
    darr_push(d->free_list, data_index);
    d->len -= 1; 
    // return the data index of the deleted entry. Caller may wish to mark data as invalid
    return data_index;
}
size_t hmap_kstr_delete(void *hmap, void *key, size_t key_size){
    return hmap__delete(hmap, key, key_size);
}
size_t hmap_kstr_get_idx(void *hmap, void *key, size_t key_size){
    return hmap__get_idx(hmap, key, key_size);
}
// len of the data array, including invalid entries. For iterating
size_t hmap_range(void *hmap){ 
    return hmap ? hmap__hdr(hmap)->len + darr_len(hmap__hdr(hmap)->free_list) : 0; 
} 

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: MURMER
// /////////////////////////////////////////////
// /////////////////////////////////////////////
// murmur3 hashing, taken from: https://github.com/PeterScott/murmur3

#ifdef __GNUC__
#define FORCE_INLINE __attribute__((always_inline)) inline
#else
#define FORCE_INLINE inline
#endif

static FORCE_INLINE u32 rotl32 (u32 x, s8 r)
{
  return (x << r) | (x >> (32 - r));
}

static FORCE_INLINE u64 rotl64 (u64 x, s8 r)
{
  return (x << r) | (x >> (64 - r));
}

#define	ROTL32(x,y)	rotl32(x,y)
#define ROTL64(x,y)	rotl64(x,y)

#define BIG_CONSTANT(x) (x##LLU)

#define getblock(p, i) (p[i])

static FORCE_INLINE u32 fmix32 (u32 h)
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;

  return h;
}

//----------

static FORCE_INLINE u64 fmix64 (u64 k)
{
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
  k ^= k >> 33;

  return k;
}
void MurmurHash3_x64_128 (const void * key, const s32 len,
                           const u32 seed, void * out)
{
  const u8 * data = (const u8*)key;
  const s32 nblocks = len / 16;
  s32 i;

  u64 h1 = seed;
  u64 h2 = seed;

  u64 c1 = BIG_CONSTANT(0x87c37b91114253d5);
  u64 c2 = BIG_CONSTANT(0x4cf5ad432745937f);

  //----------
  // body

  const u64 * blocks = (const u64 *)(data);

  for(i = 0; i < nblocks; i++)
  {
    u64 k1 = getblock(blocks,i*2+0);
    u64 k2 = getblock(blocks,i*2+1);

    k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;

    h1 = ROTL64(h1,27); h1 += h2; h1 = h1*5+0x52dce729;

    k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;

    h2 = ROTL64(h2,31); h2 += h1; h2 = h2*5+0x38495ab5;
  }

  //----------
  // tail

  const u8 * tail = (const u8*)(data + nblocks*16);

  u64 k1 = 0;
  u64 k2 = 0;

  switch(len & 15)
  {
  case 15: k2 ^= (u64)(tail[14]) << 48;
  case 14: k2 ^= (u64)(tail[13]) << 40;
  case 13: k2 ^= (u64)(tail[12]) << 32;
  case 12: k2 ^= (u64)(tail[11]) << 24;
  case 11: k2 ^= (u64)(tail[10]) << 16;
  case 10: k2 ^= (u64)(tail[ 9]) << 8;
  case  9: k2 ^= (u64)(tail[ 8]) << 0;
           k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;

  case  8: k1 ^= (u64)(tail[ 7]) << 56;
  case  7: k1 ^= (u64)(tail[ 6]) << 48;
  case  6: k1 ^= (u64)(tail[ 5]) << 40;
  case  5: k1 ^= (u64)(tail[ 4]) << 32;
  case  4: k1 ^= (u64)(tail[ 3]) << 24;
  case  3: k1 ^= (u64)(tail[ 2]) << 16;
  case  2: k1 ^= (u64)(tail[ 1]) << 8;
  case  1: k1 ^= (u64)(tail[ 0]) << 0;
           k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
  };

  //----------
  // finalization

  h1 ^= len; h2 ^= len;

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  ((u64*)out)[0] = h1;
  ((u64*)out)[1] = h2;
}

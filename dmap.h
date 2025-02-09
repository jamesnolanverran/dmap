#ifndef DMAP_H
#define DMAP_H
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DARR
// /////////////////////////////////////////////
// /////////////////////////////////////////////

// #define DMAP_DEBUG

// todo: make more configurable

#define DATA_ALIGNMENT 16

#define MAX_ARENA_CAPACITY (1024 * 1024 * 1024) 
#define DARR_INITIAL_CAPACITY 64
#define DMAP_INITIAL_CAPACITY 64
#define DARR_GROWTH_MULTIPLIER 2.0f
#define DMAP_GROWTH_MULTIPLIER 2.0f
#define DMAP_HASHTABLE_MULTIPLIER 1.6f

#ifndef SIZE_MAX
    #if defined(__x86_64__) || defined(_M_X64) || defined(_WIN64)
        #define SIZE_MAX 0xffffffffffffffffULL
    #else
        #define SIZE_MAX 0xffffffffU
    #endif
#endif

typedef enum {
    ALLOC_MALLOC,  // malloc/realloc
    ALLOC_VIRTUAL,  // reserve/commit style allocation
} AllocType;

typedef struct AllocInfo {
    char* base;
    char* ptr;
    char* end;
    size_t reserved_size;
    size_t page_size;
} AllocInfo;

typedef struct DarrHdr { 
    AllocInfo alloc_info; 
    AllocType alloc_type;
    size_t len;
    size_t cap;
    _Alignas(DATA_ALIGNMENT) char data[]; 
} DarrHdr;

static inline DarrHdr *darr__hdr(void *arr) { 
    return (DarrHdr*)( (char*)(arr) - offsetof(DarrHdr, data)); 
}
static inline size_t darr_len(void *a) { return a ? darr__hdr(a)->len : 0; }
static inline size_t darr_cap(void *a) { return a ? darr__hdr(a)->cap : 0; }
static inline void darr_clear(void *a) { if (a)  darr__hdr(a)->len = 0; }

// internal - used by macros
void *darr__grow(void *arr, size_t elem_size);
void *darr__init(void *arr, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void darr__free(void *a);

#define darr_end(a) ((a) + darr_len(a))
#define darr_free(a) ((a) ? (darr__free(a), (a) = NULL, 1) : 0) 
#define darr_fit(a, n) ((n) <= darr_cap(a) ? 0 : ((a) = darr__grow((a), sizeof(*(a)))))

#define darr_push(a, ...) (darr_fit((a), 1 + darr_len(a)), (a)[darr__hdr(a)->len] = (__VA_ARGS__), &(a)[darr__hdr(a)->len++]) // returns ptr 

// optional init: allow users to define initial capacity
// myarr = darr_init(myarr, 0); //  zero's indicates the user will use default value
#define darr_init(a, initial_capacity, alloc_type) (a) = darr__init(a, initial_capacity, sizeof(*(a)), alloc_type) 

#define darr_pop(a) ((a)[darr__hdr(a)->len-- - 1])  // it's up to the user to null check etc.
#define darr_peek(a) ((a)[darr__hdr(a)->len - 1] ) // users should write typesafe versions with checks

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

typedef struct DmapEntry { 
    size_t data_index;
    unsigned long long hash[2];  // using 128 bit murmur3
} DmapEntry;

typedef struct DmapHdr {
    AllocInfo alloc_info; 
    AllocType alloc_type;
    size_t len; 
    size_t cap;
    size_t hash_cap;
    size_t returned_idx; // stores an index, used internally by macros
    size_t *free_list; // array of indices to values stored in data[] that have been marked as deleted. 
    DmapEntry *entries; // the actual hashtable - contains the hash and an index to data[] where the values are stored
    _Alignas(DATA_ALIGNMENT) char data[];  // aligned data array - where values are stored
} DmapHdr;

// todo: ensure SIZE_MAX values below are never a valid index 

#define DMAP_ALREADY_EXISTS (SIZE_MAX)

///////////////////////
// These functions are internal but are utilized by macros so need to be declared here.
///////////////////////
static inline DmapHdr *dmap__hdr(void *d);
size_t dmap__get_idx(void *dmap, void *key, size_t key_size);
size_t dmap__delete(void *dmap, void *key, size_t key_size);
bool dmap__insert_entry(void *dmap, void *key, size_t key_size);
bool dmap__find_data_idx(void *dmap, void *key, size_t key_size);
void *dmap__grow(void *dmap, size_t elem_size) ;
void *dmap__init(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void dmap__free(void *dmap);
///////////////////////
///////////////////////
static inline DmapHdr *dmap__hdr(void *d){
    return (DmapHdr *)( (char *)d - offsetof(DmapHdr, data));
}
static inline size_t dmap_count(void *d){ return d ? dmap__hdr(d)->len : 0; } // how many valid entries in the dicctionary; not for iterating directly over the data 
static inline size_t dmap_cap(void *d){ return d ? dmap__hdr(d)->cap : 0; }

// Helper Macros - Utilized by other macros.
// =========================================
// workaround - get an index stored in d->returned_idx
#define dmap__ret_idx(d) (dmap__hdr(d)->returned_idx) // DMAP_EMPTY by default

// if the number is greater than capacity resize
#define dmap__fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = dmap__grow((d), sizeof(*(d)))))
////////////////////////////////////////////
////////////////////////////////////////////

// optionally define the initial capacity and the allocation type
#define dmap_init(d, initial_capacity, alloc_type) ((d) = dmap__init((d), initial_capacity, sizeof(*(d)), alloc_type))

// returns the index in the data array where the value is stored. If key exists returns DMAP_ALREADY_EXISTS. 
// Parameters:
// - 'd' is the hashmap from which to retrieve the value, effectively an array of v's.
// - 'k' key for the value. Keys can be any type and are not stored.
// - 'v' value 
#define dmap_insert(d, k, v) (dmap__fit((d), dmap_count(d) + 1), (dmap__insert_entry((d), (k), sizeof(*(k))) ? ((d)[dmap__ret_idx(d)] = (v)), dmap__ret_idx(d) : DMAP_ALREADY_EXISTS)) 

// same as above but uses a string as key value8
#define dmap_kstr_insert(d, k, v, key_size) (dmap__fit((d), dmap_count(d) + 1), (dmap__insert_entry((d), (k), (key_size)) ? ((d)[dmap__ret_idx(d)] = (v)), dmap__ret_idx(d) : DMAP_ALREADY_EXISTS)) 

// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define dmap_get(d,k) (dmap__find_data_idx((d), (k), sizeof(*(k))) ? &(d)[dmap__ret_idx(d)] : NULL)  

// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define dmap_kstr_get(d,k, key_size) (dmap__find_data_idx((d), (k), (key_size)) ? &(d)[dmap__ret_idx(d)] : NULL)  

// returns the data index of the deleted item or DMAP_ALREADY_EXISTS (SIZE_MAX). 
// The user should mark deleted data as invalid if the user intends to iterate over the data array.
#define dmap_delete(d,k) (dmap__delete(d, k, sizeof(*(k)))) 

// returns index to data or DMAP_ALREADY_EXISTS
// index can then be used to retrieve the value: d[idx]
#define dmap_get_idx(d,k) (dmap__get_idx(d, k, sizeof(*(k)))) 

#define dmap_free(d) ((d) ? (dmap__free(d), (d) = NULL, 1) : 0)

// same as dmap_get_idx but for keys that are strings. 
size_t dmap_kstr_get_idx(void *dmap, void *key, size_t key_size); 

// returns index to deleted data or DMAP_ALREADY_EXISTS
size_t dmap_kstr_delete(void *dmap, void *key, size_t key_size); 

// for iterating directly over the entire data array, including items marked as deleted
size_t dmap_range(void *dmap); 

void dmap_clear(void *dmap);

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: V_ALLOC
// /////////////////////////////////////////////
// /////////////////////////////////////////////

bool v_alloc_reserve(AllocInfo* alloc_info, size_t reserve_size);
void* v_alloc_committ(AllocInfo* alloc_info, size_t additional_bytes);
void v_alloc_reset(AllocInfo* alloc_info);
bool v_alloc_free(AllocInfo* alloc_info);

// set a custom error handler in case of failed allocation
void dmap_set_error_handler(void (*handler)(char *err_msg)); 

#ifdef __cplusplus
}
#endif
#endif // DMAP_H

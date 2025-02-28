#ifndef DMAP_H
#define DMAP_H
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

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

#ifndef DARR_DEFAULT_MAX_SIZE
    #define DARR_DEFAULT_MAX_SIZE (1ULL << 31) // 2GB for testing
#endif // DARR_DEFAULT_MAX_SIZE
#ifndef DMAP_DEFAULT_MAX_SIZE
    #define DMAP_DEFAULT_MAX_SIZE (1ULL << 31) // 2GB for testing
#endif // DMAP_DEFAULT_MAX_SIZE

#define MAX_ARENA_CAPACITY (1024 * 1024 * 1024) 
#define DARR_INITIAL_CAPACITY 16
#define DMAP_INITIAL_CAPACITY 16
#define DARR_GROWTH_MULTIPLIER 2.0f
#define DMAP_LOAD_FACTOR 0.5f

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
    AllocInfo *alloc_info; 
    unsigned int len;
    unsigned int cap;
    AllocType alloc_type;
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
void *dmap__kstr_grow(void *dmap, size_t elem_size);
void *darr__init(void *arr, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void *darr__kstr_init(void *arr, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void darr__free(void *a);

#define darr_end(a) ((a) + darr_len(a))
#define darr_free(a) ((a) ? (darr__free(a), (a) = NULL, 1) : 0) 
#define darr_fit(a, n) ((n) <= darr_cap(a) ? 0 : ((a) = darr__grow((a), sizeof(*(a)))))

#define darr_push(a, ...) (darr_fit((a), 1 + darr_len(a)), (a)[darr__hdr(a)->len] = (__VA_ARGS__), &(a)[darr__hdr(a)->len++]) // returns ptr 

// optional init: allow users to define initial capacity and allocation; use ALLOC_VIRTUAL for stable pointers (at some performance and memory cost) 
// myarr = darr_init(myarr, 0, ALLOC_VIRTUAL); //  zero's indicates the user will use default value
#define darr_init(a, initial_capacity, alloc_type) (a) = darr__init(a, initial_capacity, sizeof(*(a)), alloc_type) 

#define darr_pop(a) ((a)[darr__hdr(a)->len-- - 1])  // it's up to the user to null check etc.
#define darr_peek(a) ((a)[darr__hdr(a)->len - 1] ) // users should write typesafe versions with checks

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////
typedef enum {
    DMAP_UNINITIALIZED = 0,  // unused/default state
    DMAP_U64,  // 64-bit keys
    DMAP_STR   // string keys (hashed)
} KeyType;

typedef struct DmapTable DmapTable;

typedef struct DmapHdr {
    AllocInfo *alloc_info; 
    DmapTable *table; // the actual hashtable - contains the hash and an index to data[] where the values are stored
    unsigned long long hash_seed;
    unsigned int *free_list; // array of indices to values stored in data[] that have been marked as deleted. 
    unsigned int len; 
    unsigned int cap;
    unsigned int hash_cap;
    unsigned int returned_idx; // stores an index, used internally by macros
    unsigned int key_size; // make sure key sizes are consistent
    KeyType key_type;
    AllocType alloc_type;
    _Alignas(DATA_ALIGNMENT) char data[];  // aligned data array - where values are stored
} DmapHdr;

#define DMAP_INVALID SIZE_MAX

///////////////////////
// These functions are internal but are utilized by macros so need to be declared here.
///////////////////////
static inline DmapHdr *dmap__hdr(void *d);
size_t dmap__get_idx(void *dmap, void *key, size_t key_size);
size_t dmap__delete(void *dmap, void *key, size_t key_size);
void dmap__insert_entry(void *dmap, void *key, size_t key_size, bool is_string);
bool dmap__find_data_idx(void *dmap, void *key, size_t key_size);
void *dmap__grow(void *dmap, size_t elem_size) ;
void *dmap__init(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void *dmap__kstr_init(void *dmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
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
// workaround - allows macros to pass a value
#define dmap__ret_idx(d) (dmap__hdr(d)->returned_idx) // DMAP_EMPTY by default
// if the number is greater than capacity resize
#define dmap__fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = dmap__grow((d), sizeof(*(d)))))
#define dmap__kstr_fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = dmap__kstr_grow((d), sizeof(*(d)))))
////////////////////////////////////////////
////////////////////////////////////////////

// optionally define the initial capacity and the allocation type
#define dmap_init(d, initial_capacity, alloc_type) ((d) = dmap__init((d), initial_capacity, sizeof(*(d)), alloc_type))
#define dmap_kstr_init(d, initial_capacity, alloc_type) ((d) = dmap__kstr_init((d), initial_capacity, sizeof(*(d)), alloc_type))

// insert or update value
// returns the index in the data array where the value is stored.
// Parameters:
// - 'd' is the hashmap from which to retrieve the value, effectively an array of v's.
// - 'k' key for the value. Keys can be any type 1,2,4,8 bytes; use dmap_kstr_insert for strings and non-builtin types
// - 'v' value 
#define dmap_insert(d, k, v) (dmap__fit((d), dmap_count(d) + 1), dmap__insert_entry((d), (k), sizeof(*(k)), false), ((d)[dmap__ret_idx(d)] = (v)), dmap__ret_idx(d)) 
// same as above but uses a string as key values
#define dmap_kstr_insert(d, k, v, key_size) (dmap__kstr_fit((d), dmap_count(d) + 1), dmap__insert_entry((d), (k), (key_size), true), ((d)[dmap__ret_idx(d)] = (v)), dmap__ret_idx(d)) 

// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found. Use ALLOC_VIRTUAL for stable pointers (at some performance and memory cost) 
#define dmap_get(d,k) (dmap__find_data_idx((d), (k), sizeof(*(k))) ? &(d)[dmap__ret_idx(d)] : NULL)  
// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define dmap_kstr_get(d,k, key_size) (dmap__find_data_idx((d), (k), (key_size)) ? &(d)[dmap__ret_idx(d)] : NULL)  

// returns the data index of the deleted item or DMAP_INVALID (SIZE_MAX). 
// The user should mark deleted data as invalid if the user intends to iterate over the data array.
#define dmap_delete(d,k) (dmap__delete(d, k, sizeof(*(k)))) 
// returns index to deleted data or DMAP_INVALID
size_t dmap_kstr_delete(void *dmap, void *key, size_t key_size); 

// returns index to data or DMAP_INVALID; indices are always stable
// index can then be used to retrieve the value: d[idx]
#define dmap_get_idx(d,k) (dmap__get_idx(d, k, sizeof(*(k)))) 
// same as dmap_get_idx but for keys that are strings. 
size_t dmap_kstr_get_idx(void *dmap, void *key, size_t key_size); 

#define dmap_free(d) ((d) ? (dmap__free(d), (d) = NULL, 1) : 0)

// for iterating directly over the entire data array, including items marked as deleted
size_t dmap_range(void *dmap); 


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

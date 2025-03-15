#ifndef DMAP_H
#define DMAP_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// #define DMAP_DEBUG

// todo: make more configurable

#ifndef DMAP_ALIGNMENT
    #define DMAP_ALIGNMENT 16
#endif

#ifndef DMAP_DEFAULT_MAX_SIZE
    // 2GB for testing
    #define DMAP_DEFAULT_MAX_SIZE (1ULL << 31) 
#endif // DMAP_DEFAULT_MAX_SIZE

#define DMAP_INITIAL_CAPACITY 16
#define DMAP_LOAD_FACTOR 0.5f

typedef enum {
    DMAP_U64,  // up to 64-bit keys
    DMAP_STR   // string keys etc (double hashed)
} KeyType;

typedef struct DmapFreeList {
    unsigned int *data;
    unsigned int len;
    unsigned int cap;
} DmapFreeList;

typedef struct DmapTable DmapTable;

typedef void *(*AllocatorFn)(void *hdr, size_t new_total_size);

typedef struct DmapHdr {
    DmapTable *table; // the actual hashtable - contains the hash and an index to data[] where the values are stored
    AllocatorFn alloc; // allocator must provide a single contiguous block of memory
    unsigned long long hash_seed;
    DmapFreeList *free_list; // array of indices to values stored in data[] that have been marked as deleted. 
    unsigned int len; 
    unsigned int cap;
    unsigned int hash_cap;
    unsigned int returned_idx; // stores an index, used internally by macros
    unsigned int key_size; // make sure key sizes are consistent
    unsigned int val_size;
    KeyType key_type;
    _Alignas(DMAP_ALIGNMENT) char data[];  // aligned data array - where values are stored
} DmapHdr;

#define DMAP_INVALID SIZE_MAX

///////////////////////
// These functions are internal but are utilized by macros so need to be declared here.
///////////////////////
static inline DmapHdr *dmap__hdr(void *d);
size_t dmap__get_idx(void *dmap, void *key, size_t key_size);
size_t dmap__delete(void *dmap, void *key, size_t key_size);
void dmap__insert_entry(void *dmap, void *key, size_t key_size);
void *dmap__find_data_idx(void *dmap, void *key, size_t key_size);
void *dmap__grow(void *dmap, size_t elem_size) ;
void *dmap__kstr_grow(void *dmap, size_t elem_size);
void *dmap__init(void *dmap, size_t initial_capacity, size_t elem_size, AllocatorFn alloc);
void *dmap__kstr_init(void *dmap, size_t initial_capacity, size_t elem_size, AllocatorFn alloc);
void dmap__free(void *dmap);
///////////////////////
///////////////////////
static inline DmapHdr *dmap__hdr(void *d){
    return (DmapHdr *)( (char *)d - offsetof(DmapHdr, data));
}
static inline size_t dmap_count(void *d){ return d ? dmap__hdr(d)->len : 0; } // how many valid entries in the dicctionary; not for iterating directly over the data 
static inline size_t dmap_cap(void *d){ return d ? dmap__hdr(d)->cap : 0; }

// Helper Macros - Utilized by other macros.

// allows macros to pass a value
#define dmap__ret_idx(d) (dmap__hdr(d)->returned_idx) // DMAP_EMPTY by default

// resize if n <= capacity
#if defined(__cplusplus)
    #define dmap__fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = (decltype((d) + 0))dmap__grow((d), sizeof(*(d)))))
#else
    #define dmap__fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = dmap__grow((d), sizeof(*(d)))))
#endif
#if defined(__cplusplus)
    #define dmap__kstr_fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = (decltype((d) + 0))dmap__kstr_grow((d), sizeof(*(d)))))
#else
    #define dmap__kstr_fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = dmap__kstr_grow((d), sizeof(*(d)))))
#endif
////////////////////////////////////////////

// optionally define the initial capacity and/or the allocator function
// AllocatorFn alloc; // allocator must provide a single contiguous block of memory
#define dmap_init(d, initial_capacity, alloc) ((d) = dmap__init((d), initial_capacity, sizeof(*(d)), alloc))
#define dmap_kstr_init(d, initial_capacity, alloc) ((d) = dmap__kstr_init((d), initial_capacity, sizeof(*(d)), alloc))

// insert or update value
// returns the index in the data array where the value is stored.
// Parameters:
// - 'd' is the hashmap from which to retrieve the value, effectively an array of v's.
// - 'k' key for the value. Keys can be any type 1,2,4,8 bytes; use dmap_kstr_insert for strings and non-builtin types
// - 'v' value -> VAR_ARGS to allow for direct struct initialization: dmap_kstr_insert(d, k, key_size, (MyType){2,33});
#define dmap_insert(d, k, ...) (dmap__fit((d), dmap_count(d) + 1), dmap__insert_entry((d), (k), sizeof(*(k))), ((d)[dmap__ret_idx(d)] = (__VA_ARGS__)), dmap__ret_idx(d)) 
// same as above but uses a string as key values
#define dmap_kstr_insert(d, k, key_size, ...) (dmap__kstr_fit((d), dmap_count(d) + 1), dmap__insert_entry((d), (k), (key_size)), ((d)[dmap__ret_idx(d)] = (__VA_ARGS__)), dmap__ret_idx(d)) 

// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found. 
#if defined(__cplusplus) // haven't tested
    #define dmap_get(d, k) ((decltype(d)) dmap__find_data_idx((d), (k), sizeof(*(k))))
#elif defined(__clang__) || defined(__GNUC__)  
    #define dmap_get(d, k) ((typeof(d)) dmap__find_data_idx((d), (k), sizeof(*(k))))
#else // fallback
    #define dmap_get(d, k) (dmap__find_data_idx((d), (k), sizeof(*(k))))
#endif
// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#if defined(__cplusplus)// haven't tested
    #define dmap_kstr_get(d, k, key_size) ((decltype(d)) dmap__find_data_idx((d), (k), (key_size)))
#elif defined(__clang__) || defined(__GNUC__)  
    #define dmap_kstr_get(d, k, key_size) ((typeof(d)) dmap__find_data_idx((d), (k), (key_size)))
#else // fallback
    #define dmap_kstr_get(d, k, key_size) (dmap__find_data_idx((d), (k), (key_size)))
#endif

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

#ifdef __cplusplus
}
#endif

#endif // DMAP_H

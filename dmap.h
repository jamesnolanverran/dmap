#ifndef DMAP_H
#define DMAP_H
#include <stddef.h>
#include <stdbool.h>

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

typedef struct DmapFreeList {
    int *data;
    int len;
    int cap;
} DmapFreeList;

typedef struct DmapTable DmapTable;

typedef struct DmapOptions {
    void *(*data_allocator_fn)(void *hdr, size_t size); // custom allocator for the data array (default: realloc)
    void (*free_key_fn)(void*);  // custom free function for keys
    unsigned long long (*hash_fn)(void *key, size_t len);
    bool (*cmp_fn)(void *a, void *b, size_t len);
    int initial_capacity;  
    bool user_managed_keys;  // if true, the user manages string keys; otherwise, dmap copies and frees them on delete
} DmapOptions;

typedef struct DmapHdr {
    DmapTable *table; // the actual hashtable - contains the hash and an index to data[] where the values are stored
    unsigned long long hash_seed;
    DmapFreeList *free_list; // array of indices to values stored in data[] that have been marked as deleted. 
    DmapOptions options;
    int len; 
    int cap;
    int hash_cap;
    int returned_idx; // stores an index, used internally by macros
    int key_size; // make sure key sizes are consistent
    int val_size;
    bool is_string;
    _Alignas(DMAP_ALIGNMENT) char data[];  // aligned data array - where values are stored
} DmapHdr;

#define DMAP_INVALID -1

#if defined(__cplusplus)
    #define DMAP_TYPEOF(d) (decltype((d) + 0))
#elif defined(__clang__) || defined(__GNUC__)  
    #define DMAP_TYPEOF(d) (typeof(d))
#else
    #define DMAP_TYPEOF(d)
#endif
///////////////////////
// These functions are internal but are utilized by macros so need to be declared here.
///////////////////////
int dmap__get_idx(DmapHdr *d, void *key, size_t key_size);
int dmap__delete(DmapHdr *d, void *key, size_t key_size);
void dmap__insert_entry(DmapHdr *d, void *key, size_t key_size);
void *dmap__getp(DmapHdr *d, void *key, size_t key_size);
void *dmap__grow(DmapHdr *d, size_t elem_size) ;
void *dmap__kstr_grow(DmapHdr *d, size_t elem_size);
void *dmap__init(size_t elem_size, DmapOptions options);
void *dmap__kstr_init(size_t elem_size, DmapOptions options);

void dmap__free(DmapHdr *d);

///////////////////////
#define dmap_hdr(d) ((DmapHdr *)((char *)(d) - offsetof(DmapHdr, data)))
#define dmap_count(d) ((d) ? dmap_hdr(d)->len : 0) // how many valid entries in the dicctionary; not for iterating directly over the data 
#define dmap_cap(d)   ((d) ? dmap_hdr(d)->cap : 0)

unsigned long long dmap_hash(void *key, size_t key_size);

// Helper Macros - Utilized by other macros.
////////////////////////////////////////////
// allows macros to pass a value
#define dmap__ret_idx(d) (dmap_hdr(d)->returned_idx) // DMAP_EMPTY by default
// resize if n <= capacity
#define dmap__fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = DMAP_TYPEOF(d) dmap__grow((d) ? dmap_hdr(d) : NULL, sizeof(*(d)))))
#define dmap__kstr_fit(d, n) ((n) <= dmap_cap(d) ? 0 : ((d) = DMAP_TYPEOF(d) dmap__kstr_grow((d) ? dmap_hdr(d) : NULL, sizeof(*(d)))))
////////////////////////////////////////////

// dmap_init(d, DmapOptions opts)
// ex: dmap_kstr_init(dmap, (DmapOptions){.initial_capacity = 256});
#define dmap_init(d, ...)((d) = DMAP_TYPEOF(d) dmap__init(sizeof(*(d)), __VA_ARGS__));
#define dmap_kstr_init(d, ...)((d) = DMAP_TYPEOF(d) dmap__kstr_init(sizeof(*(d)), __VA_ARGS__));

// insert or update value
// returns the index in the data array where the value is stored.
// Parameters:
// - 'd' is the hashmap from which to retrieve the value, effectively an array of v's.
// - 'k' key for the value. Keys can be any type 1,2,4,8 bytes; use dmap_kstr_insert for strings and non-builtin types
// - 'v' value -> VAR_ARGS to allow for direct struct initialization: dmap_kstr_insert(d, k, key_size, (MyType){2,33});
#define dmap_insert(d, k, ...) (dmap__fit((d), dmap_count(d) + 1), dmap__insert_entry(dmap_hdr(d), (k), sizeof(*(k))), ((d)[dmap__ret_idx(d)] = (__VA_ARGS__)), dmap__ret_idx(d)) 
// same as above but uses a string as key values
#define dmap_kstr_insert(d, k, key_size, ...) (dmap__kstr_fit((d), dmap_count(d) + 1), dmap__insert_entry(dmap_hdr(d), (k), (key_size)), ((d)[dmap__ret_idx(d)] = (__VA_ARGS__)), dmap__ret_idx(d)) 

// returns index to data or -1 / DMAP_INVALID; indices are always stable
// index can then be used to retrieve the value: d[idx]
#define dmap_get(d,k) ((d) ? dmap__get_idx(dmap_hdr(d), (k), sizeof(*(k))) : -1) 
// same as dmap_get but for keys that are strings. 
#define dmap_kstr_get(d, k, key_size)((d) ? dmap__get_idx(dmap_hdr(d), (k), (key_size)) : -1)

// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found. 
#define dmap_getp(d, k) ((d) ? DMAP_TYPEOF(d) dmap__getp(dmap_hdr(d), (k), sizeof(*(k))) : NULL)
// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define dmap_kstr_getp(d, k, key_size) ((d) ? DMAP_TYPEOF(d) dmap__getp(dmap_hdr(d), (k), (key_size)) : NULL)

// returns the data index of the deleted item or -1 / DMAP_INVALID (SIZE_MAX). 
// The user should mark deleted data as invalid if the user intends to iterate over the data array.
#define dmap_kstr_delete(d, k, len)((d) ? dmap__delete(dmap_hdr(d), (k), (len)) : -1)

#define dmap_delete(d,k) ((d) ? dmap__delete(dmap_hdr(d), (k), sizeof(*(k))) : -1) 
// returns index to deleted data or -1 / DMAP_INVALID


#define dmap_free(d) ((d) ? (dmap__free(dmap_hdr(d)), (d) = NULL, 1) : 0)

// for iterating directly over the entire data array, including items marked as deleted
int dmap__range(DmapHdr *d); 
#define dmap_range(d)(dmap__range((d) ? dmap_hdr(d) : NULL))

#ifdef __cplusplus
}
#endif

#endif // DMAP_H

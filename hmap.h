#ifndef HMAP_H
#define HMAP_H
#include <stdbool.h>
#include <stddef.h>

// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: DARR
// /////////////////////////////////////////////
// /////////////////////////////////////////////

#define DEBUG

#define DATA_ALIGNMENT 16
#define MAX_ARENA_CAPACITY 1024 * 1024 * 1024 // 1GB

#define DARR_INITIAL_CAPACITY 64 
#define HMAP_INITIAL_CAPACITY 64

#define DARR_GROWTH_MULTIPLIER 2.0f

#define HMAP_GROWTH_MULTIPLIER 2.0f
#define HMAP_HASHTABLE_MULTIPLIER 1.6f


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
// MARK: HMAP
// /////////////////////////////////////////////
// /////////////////////////////////////////////

typedef struct HmapEntry { 
    size_t data_index;
    unsigned long long hash[2];  // using 128 bit murmur3
} HmapEntry;

typedef struct HMapHdr {
    AllocInfo alloc_info; 
    AllocType alloc_type;
    size_t len; 
    size_t cap;
    size_t hash_cap;
    size_t returned_idx; // stores an index used by macros
    size_t *free_list; // array of indices to values stored in data[] that have been marked as deleted. 
    HmapEntry *entries; // the actual hashtable - contains the hash and an index to data[] where the values are stored
    _Alignas(DATA_ALIGNMENT) char data[];  // aligned data array - where values are stored
} HMapHdr;

// todo: ensure SIZE_MAX values below are never a valid index 
#define HMAP_EMPTY (SIZE_MAX)
#define HMAP_DELETED (SIZE_MAX - 1)

#define HMAP_ALREADY_EXISTS (SIZE_MAX)

///////////////////////
// These functions are internal but are utilized by macros so need to be declared here.
///////////////////////
static inline HMapHdr *hmap__hdr(void *d);
size_t hmap__get_idx(void *hmap, void *key, size_t key_size);
size_t hmap__delete(void *hmap, void *key, size_t key_size);
bool hmap__insert_entry(void *hmap, void *key, size_t key_size);
bool hmap__find_data_idx(void *hmap, void *key, size_t key_size);
void *hmap__grow(void *hmap, size_t elem_size) ;
void *hmap__init(void *hmap, size_t initial_capacity, size_t elem_size, AllocType alloc_type);
void hmap__free(void *hmap);
///////////////////////
///////////////////////
static inline HMapHdr *hmap__hdr(void *d){
    return (HMapHdr *)( (char *)d - offsetof(HMapHdr, data));
}
static inline size_t hmap_count(void *d){ return d ? hmap__hdr(d)->len : 0; } // how many valid entries in the dicctionary; not for iterating directly over the data 
static inline size_t hmap_cap(void *d){ return d ? hmap__hdr(d)->cap : 0; }

// Helper Macros - Utilized by other macros.
// =========================================
#define hmap__ret_idx(d) (hmap__hdr(d)->returned_idx) // NOT_FOUND or HMAP_EMPTY by default
// hmap__entries: Retrieves the entries of the hmapionary 'd'. 
// Note: caller should ensure 'd' is valid
#define hmap__entries(d) (hmap__hdr(d)->entries)
// hmap__idx_to_val: Retrieves the value corresponding to the index 'idx' in hmapionary 'd'. 
// Note: caller should ensure 'd' is valid
#define hmap__idx_to_val(d,idx) ( (d)[ hmap__entries(d)[(idx)].data_index ] )  
// if the number of entries is more than half the size of the capacity, double the capacity.
#define hmap__fit(d, n) ((n) <= hmap_cap(d) ? 0 : ((d) = hmap__grow((d), sizeof(*(d)))))
////////////////////////////////////////////
////////////////////////////////////////////

#define hmap_init(d, initial_capacity, alloc_type) ((d) = hmap__init((d), initial_capacity, sizeof(*(d)), alloc_type))
// returns the index in the data array where the value is stored. If key exists returns NOT_FOUND. 
// param - d: pointer to array of v's
//         k: ptr to key of any type
//         v: value of any chosen type
#define hmap_insert(d, k, v) (hmap__fit((d), hmap_count(d) + 1), (hmap__insert_entry((d), (k), sizeof(*(k))) ? ((d)[hmap__ret_idx(d)] = (v)), hmap__ret_idx(d) : HMAP_ALREADY_EXISTS)) 
// same as above but uses a string as key value8
#define hmap_kstr_insert(d, k, v, key_size) (hmap__fit((d), hmap_count(d) + 1), (hmap__insert_entry((d), (k), (key_size)) ? ((d)[hmap__ret_idx(d)] = (v)), hmap__ret_idx(d) : HMAP_ALREADY_EXISTS)) 

// hmap_get: Retrieves a pointer to the value associated with the key 'k' in hashmap 'd'.
// Parameters:
// - 'd' is the hashmap from which to retrieve the value.
// - 'k' is the key for the value. The address of 'k' and its size (using sizeof) are used in the lookup.
// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define hmap_get(d,k) (hmap__find_data_idx((d), (k), sizeof(*(k))) ? &(d)[hmap__ret_idx(d)] : NULL)  
// hmap_kstr_get: Retrieves a pointer to the value associated with a C-string key 'k' in hashmap 'd'.
// Parameters:
// - 'd' is the hashmap from which to retrieve the value.
// - 'k' is the C-string key for the value.
// - 'key_size' is the size of the key 'k'.
// Returns: A pointer to the value corresponding to 'k' in 'd', or NULL if the key is not found.
#define hmap_kstr_get(d,k, key_size) (hmap__find_data_idx((d), (k), (key_size)) ? &(d)[hmap__ret_idx(d)] : NULL)  

// Useful? untested
#define hmap_get_or_set_default(d,k,v) ( hmap__find_data_idx((d), (k), sizeof(*(k))) ?               \
                               ( &hmap__idx_to_val((d), hmap__ret_idx(d))) :                     \
                               ( (hmap_insert((d), (k), (v))), hmap_get_ptr((d), (k))) )

// returns the data index of the deleted item or NOT_FOUND / SIZE_MAX. The user should mark deleted 
// data as invalid in some way if the user intends to iterate over the data array.
#define hmap_delete(d,k) (hmap__delete(d, k, sizeof(*(k)))) // returns index to deleted data or NOT_FOUND on fail

#define hmap_get_idx(d,k) (hmap__get_idx(d, k, sizeof(*(k)))) // returns index to data or NOT_FOUND

#define hmap_free(d) ((d) ? (hmap__free(d), (d) = NULL, 1) : 0)


size_t hmap_kstr_get_idx(void *hmap, void *key, size_t key_size); // same as hmap_get but for keys that are strings. 
size_t hmap_kstr_delete(void *hmap, void *key, size_t key_size); // returns index to deleted data or NOT_FOUND
size_t hmap_range(void *hmap); // for iterating directly over the entire data array, including items marked as deleted
void hmap_clear(void *hmap);


// /////////////////////////////////////////////
// /////////////////////////////////////////////
// MARK: V_ALLOC
// /////////////////////////////////////////////
// /////////////////////////////////////////////

bool v_alloc_reserve(AllocInfo* alloc_info, size_t reserve_size);
void* v_alloc_committ(AllocInfo* alloc_info, size_t additional_bytes);
void v_alloc_reset(AllocInfo* alloc_info);
bool v_alloc_free(AllocInfo* alloc_info);


void hmap_set_error_handler(void (*handler)(char *err_msg)); // set a custom error handler in case of failed allocation

#endif // HMAP_H

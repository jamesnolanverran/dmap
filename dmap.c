#include "dmap.h"
#include <stdbool.h>
#include <stdlib.h> 
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef DMAP_DEBUG
    #if defined(_MSC_VER) || defined(_WIN32)
        #define DEBUGBREAK() __debugbreak()
    #else
        #define DEBUGBREAK() __builtin_trap()
    #endif

    #ifndef dmap_assert
        #define dmap_assert(expr)                              \
            do {                                               \
                if (!(expr)) {                                 \
                    common_assert_failed(__FILE__, __LINE__);  \
                }                                              \
            } while (0)
    #endif // dmap_assert

    #ifndef common_assert_failed
        #define common_assert_failed(f, l)                     \
            do {                                               \
                printf("assert failed at file %s(%d)", f, l);  \
                DEBUGBREAK();                                  \
            } while (0)
    #endif // common_assert_failed
#else
    #define dmap_assert
#endif

#if defined(_MSC_VER) || defined(_WIN32)
    #include <intrin.h>  // MSVC intrinsics
#endif

static inline size_t next_power_of_2(size_t x) {
    if (x <= 1) return 1;  // ensure minimum value of 1

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
// 
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
// MARK: ERR HANDLER
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
// MARK: DMAP
// /////////////////////////////////////////////

struct DmapTable {
    u64 hash;
    union {
        u64 key;
        void *ptr;
        char *kstr;
        char small_kstr[8];
    };
    s32 data_idx;
    s32 kstr_len;
};

#define DMAP_EMPTY   INT32_MAX
#define DMAP_DELETED (INT32_MAX - 1)
#define DMAP_MAX_CAPACITY ((size_t)INT32_MAX - 2)


// declare hash functions
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
static inline u64 rapidhash_internal(const void *key, size_t len, u64 seed, const u64 *secret) RAPIDHASH_NOEXCEPT;
static inline u64 rapidhash(const void *key, size_t len) RAPIDHASH_NOEXCEPT;

static const u64 RAPIDHASH_SECRET[3] = {
    0x9E3779B97F4A7C15ULL,  
    0xD6E8FEB86659FD93ULL,  
    0xCA9B0C7EBA1DA115ULL   
};

unsigned long long dmap_hash(void *key, size_t len){
    return rapidhash(key, len);
}
static unsigned long long dmap_generate_hash(void *key, size_t key_size, unsigned long long seed) {
    return rapidhash_internal(key, key_size, seed, RAPIDHASH_SECRET);
}

static void dmap_freelist_push(DmapHdr *dh, s32 index) {
    if(!dh->free_list){
        dh->free_list = (DmapFreeList*)malloc(sizeof(DmapFreeList));
        if(!dh->free_list){
            dmap_error_handler("malloc failed at freelist");
        }
        dh->free_list->cap = 16;
        dh->free_list->len = 0;
        dh->free_list->data = (s32*)malloc(dh->cap * sizeof(s32));
        if(!dh->free_list->data){
            dmap_error_handler("malloc failed at freelist");
        }
    }
    if (dh->free_list->len == dh->free_list->cap) {
        dh->free_list->cap = (dh->free_list->cap * 3) / 2 + 1;  
        dh->free_list->data = (s32*)realloc(dh->free_list->data, dh->free_list->cap * sizeof(s32));
        if(!dh->free_list->data){
            dmap_error_handler("realloc failed at freelist");
        }
    }
    dh->free_list->data[dh->free_list->len++] = index;
}
static u32 dmap_freelist_pop(DmapHdr *dh) {
    if (dh->free_list && dh->free_list->len > 0) {
        return dh->free_list->data[--dh->free_list->len];
    }
    return DMAP_EMPTY;  // no free slots available
}
static bool keys_match(DmapHdr *d, size_t idx, void *key, size_t key_size) {
    if (d->is_string && d->table[idx].kstr_len != (s32)key_size) {
        return false;
    }
    void *stored = (!d->options.user_managed_keys && key_size <= 8)
        ? (void*)&d->table[idx].key
        : d->table[idx].ptr;
    if (d->options.cmp_fn) {
        return d->options.cmp_fn(stored, key, key_size);
    }
    return memcmp(stored, key, key_size) == 0;
}

// grows the entry array of the hashmap to accommodate more elements
static void dmap_grow_table(DmapHdr *d, size_t new_hash_cap, size_t old_hash_cap) {
    // size_t new_size_in_bytes = new_hash_cap * sizeof(DmapTable);
    DmapTable *new_table = (DmapTable*)calloc(new_hash_cap, sizeof(DmapTable));
    if (!new_table) {
        dmap_error_handler("Out of memory 1");
    }
    for(size_t i = 0; i < new_hash_cap; i++){
        new_table[i].data_idx = DMAP_EMPTY;
    }
    // if the hashmap has existing table, rehash them into the new entry array
    if (d->len) {
        for (size_t i = 0; i < old_hash_cap; i++) {
            if(d->table[i].data_idx == DMAP_EMPTY) continue;
            size_t idx = d->table[i].hash & (new_hash_cap - 1);
            // size_t j = new_hash_cap;
            while(true){
                // dmap_assert(j-- != 0); // unreachable, suggests no empty slot was found
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
static void *dmap__grow_internal(DmapHdr *d, size_t elem_size) {
    DmapHdr *new_hdr = NULL;
    size_t old_hash_cap = d->hash_cap;
    size_t new_hash_cap = old_hash_cap * 2;
    size_t new_cap = (size_t)((float)new_hash_cap * DMAP_LOAD_FACTOR);
    size_t total_size_in_bytes = offsetof(DmapHdr, data) + (new_cap * elem_size);
    if (new_cap > DMAP_MAX_CAPACITY) {
        dmap_error_handler("Error: Max capacity exceeded.\n");
    }
    if(total_size_in_bytes > DMAP_DEFAULT_MAX_SIZE){
        dmap_error_handler("Error: Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overide default.");
    }

    new_hdr = (DmapHdr*)d->options.data_allocator_fn(d, total_size_in_bytes);

    if(!new_hdr) {
        dmap_error_handler("Out of memory 2");
    }
    // grow the table to fit into the newly allocated space
    dmap_grow_table(new_hdr, new_hash_cap, old_hash_cap); 

    new_hdr->cap = (u32)new_cap;
    new_hdr->hash_cap = (u32)new_hash_cap;

    dmap_assert(((uintptr_t)&new_hdr->data & (DMAP_ALIGNMENT - 1)) == 0); // ensure alignment
    return new_hdr->data; // return the aligned data pointer
}

static void *dmap__init_internal(size_t elem_size, bool is_string, DmapOptions options){
    DmapHdr *new_hdr = NULL;

    s32 capacity = options.initial_capacity;
    size_t table_capacity = next_power_of_2(capacity);
    while ((size_t)((float)table_capacity * DMAP_LOAD_FACTOR) < (size_t)capacity) {
        if (table_capacity > SIZE_MAX / 2) {  // Prevent overflow
            dmap_error_handler("Error: exceeded max capacity");
        }
        table_capacity *= 2;
    }
    capacity = (size_t)((float)table_capacity * DMAP_LOAD_FACTOR);
    size_t size_in_bytes = offsetof(DmapHdr, data) + (capacity * elem_size);
    if(capacity > DMAP_MAX_CAPACITY){
        dmap_error_handler("Error: Max capacity exceeded.\n");
    }
    if(size_in_bytes > DMAP_DEFAULT_MAX_SIZE){
        dmap_error_handler("Error: Max size exceeded. #define DMAP_DEFAULT_MAX_SIZE to overide default.");
    }
    if(!options.data_allocator_fn){
        options.data_allocator_fn = realloc;
    }
    new_hdr = (DmapHdr*)options.data_allocator_fn(NULL, size_in_bytes);
    if(!new_hdr){
        dmap_error_handler("Out of memory 3");
    }
    if(options.free_key_fn){
        options.user_managed_keys = true;
    }
    new_hdr->options = options;
    new_hdr->len = 0;
    new_hdr->cap = (u32)capacity;
    new_hdr->hash_cap = (u32)table_capacity;
    new_hdr->returned_idx = DMAP_EMPTY;
    new_hdr->table = NULL;
    new_hdr->free_list = NULL;
    new_hdr->key_size = 0;
    new_hdr->val_size = (u32)elem_size;
    new_hdr->hash_seed = dmap_generate_seed();
    new_hdr->is_string = is_string;

    dmap_grow_table(new_hdr, new_hdr->hash_cap, 0);
    dmap_assert(((uintptr_t)&new_hdr->data & (DMAP_ALIGNMENT - 1)) == 0); // ensure alignment
    return new_hdr->data;
}
void *dmap__init(size_t elem_size, DmapOptions options){
    return dmap__init_internal(elem_size, false, options);
}
void *dmap__kstr_init(size_t elem_size, DmapOptions options){
    return dmap__init_internal(elem_size, true, options);
}
static inline DmapOptions dmap_default_options(void) {
    return (DmapOptions){
        .data_allocator_fn = NULL,
        .free_key_fn = NULL,
        .initial_capacity = DMAP_INITIAL_CAPACITY,
        .user_managed_keys = false,
    };
}
// grows the hashmap to a new capacity
void *dmap__grow(DmapHdr *d, size_t elem_size) {
    if (!d) {
        // when this is the case we just want the defaults
        return dmap__init(elem_size, dmap_default_options());
    }
    return dmap__grow_internal(d, elem_size);
}
void *dmap__kstr_grow(DmapHdr *d, size_t elem_size) {
    if (!d) {
        // when this is the case we just want the defaults
        return dmap__kstr_init(elem_size, dmap_default_options());
    }
    return dmap__grow_internal(d, elem_size);
}
void dmap__free(DmapHdr *d){
    if(d){
        if(d->table) {
            if(d->options.user_managed_keys && d->options.free_key_fn){ // user manages keys & provided a free function
                for(s32 i = 0; i < d->hash_cap; i++){
                    if(d->table[i].ptr != NULL){
                        d->options.free_key_fn(d->table[i].ptr);
                    }
                }
            }
            else if(!d->options.user_managed_keys){
                if(d->is_string){
                    for(s32 i = 0; i < d->hash_cap; i++){
                        if(d->table[i].kstr_len > 8 && d->table[i].kstr != NULL){
                            free(d->table[i].kstr);
                        }
                    }
                }
                else {
                    for(s32 i = 0; i < d->hash_cap; i++){
                        if(d->key_size > 8 && d->table[i].ptr != NULL){
                            free(d->table[i].ptr);
                        }
                    }
                }
            }
            free(d->table); 
        }
        if(d->free_list){
            if(d->free_list->data) {
                free(d->free_list->data);
            }
            free(d->free_list);
        }
        d->options.data_allocator_fn(d, 0); 
    }
}
s32 dmap__get_entry_index(DmapHdr *d, void *key, size_t key_size){
    s32 result = DMAP_INVALID;
    if(d->cap != 0) {
        u64 hash = d->options.hash_fn ? d->options.hash_fn(key, key_size) : dmap_generate_hash(key, key_size, d->hash_seed); // generate a hash value for the given key
        s32 idx = hash & (d->hash_cap - 1);
        // size_t j = d->hash_cap; // counter to ensure the loop doesn't iterate more than the capacity of the hashmap
        while(true) { // loop to search for the key in the hashmap
            // dmap_assert(j-- != 0); // unreachable -- suggests table is full
            if(d->table[idx].data_idx == DMAP_EMPTY){ // if the entry is empty, the key is not in the hashmap
                break;
            }
            if(d->table[idx].data_idx != DMAP_DELETED && d->table[idx].hash == hash) {
                if(keys_match(d, idx, key, key_size)){
                    result = idx;
                    break;
                }
            }
            idx = (idx + 1) & (d->hash_cap - 1); // move to the next index, wrapping around to the start if necessary
        }
    }
    return result;
}
static char *dmap_strdup(char *src, size_t len) {
    char *dst = (char*)malloc(len + 1);
    if (!dst) {
        return NULL; 
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}
static void *dmap_dup_struct(const void *src, size_t len) {
    void *dst = malloc(len);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len);
    return dst;
}


void dmap__insert_entry(DmapHdr *d, void *key, size_t key_size){ 
    if(d->key_size == 0){  
        if(d->is_string) 
            d->key_size = -1; // strings
        else 
            d->key_size = (s32)key_size;
    }
    else if(d->key_size != (s32)key_size && d->key_size != -1){
        dmap_error_handler("Error: key is not the correct size");
    }
    u64 hash =  d->options.hash_fn ? d->options.hash_fn(key, key_size) : dmap_generate_hash(key, key_size, d->hash_seed);
    u32 idx = hash & (d->hash_cap - 1);
    // size_t j = d->hash_cap;
    while(true){
        // dmap_assert(j-- != 0); // unreachable - suggests there were no empty slots
        if(d->table[idx].data_idx == DMAP_EMPTY || d->table[idx].data_idx == DMAP_DELETED){ // insert in empty or deleted
            break;
        }
        if(d->table[idx].hash == hash){ 
            if(keys_match(d, idx, key, key_size)){ // modify existing entry
                break;
            }
        }
        idx = (idx + 1) & (d->hash_cap - 1);
    }
    if(d->table[idx].data_idx != DMAP_EMPTY && d->table[idx].data_idx != DMAP_DELETED){
        d->returned_idx = d->table[idx].data_idx;
    }
    else {

        d->returned_idx = d->free_list && d->free_list->len > 0 ? dmap_freelist_pop(d) : d->len;
        d->len += 1;

        DmapTable *entry = &d->table[idx];
        entry->hash = hash;
        entry->data_idx = d->returned_idx;
        if(d->is_string) { 
            entry->kstr_len = (s32)key_size;
            if(d->options.user_managed_keys) { // user managed/allocated keys
                entry->kstr = key;
            }
            else if(key_size <= 8) { // otherwise dmap copies string keys
                memcpy(entry->small_kstr, key, key_size);
            }
            else {
                entry->kstr = dmap_strdup(key, key_size);
                if(!entry->kstr){
                    dmap_error_handler("Error: dmap_strdup - malloc failed");
                }
            }
        }
        else {
            dmap_assert(d->key_size == (s32)key_size);
            entry->kstr_len = (s32)key_size;
            if(d->options.user_managed_keys) { // user managed/allocated keys
                entry->ptr = key;
            }
            else if(key_size <= 8){ // copy small keys directly to table
                entry->key = 0;  // zero-out first
                memcpy(&entry->key, key, key_size);
            }
            else {
                entry->ptr = dmap_dup_struct(key, key_size); // allocate copies > 8 bytes
                if(!entry->ptr){
                    dmap_error_handler("Error: dmap_dup_struct - malloc failed");
                }
            }
        }
    }
    return;
}

void* dmap__getp(DmapHdr *d, void *key, size_t key_size){
    if(d->key_size != (s32)key_size && d->key_size != -1){  // -1 indicates a string key
        dmap_error_handler("Error: key is not the correct size");
    }
    size_t idx = dmap__get_entry_index(d, key, key_size);
    if(idx == DMAP_INVALID) { 
        return NULL; // entry is not found
    }
    return d->data + d->table[idx].data_idx * d->val_size;
}
// returns: int - The index of the data associated with the key, or DMAP_INVALID (-1) if the key is not found
s32 dmap__get_idx(DmapHdr *d, void *key, size_t key_size){
    s32 idx = dmap__get_entry_index(d, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    return d->table[idx].data_idx;
}

    // returns the data index of the deleted entry. Caller may wish to mark data as invalid
s32 dmap__delete(DmapHdr *d, void *key, size_t key_size){
    s32 idx = dmap__get_entry_index(d, key, key_size);
    if(idx == DMAP_INVALID) {
        return DMAP_INVALID;
    }
    s32 data_index = d->table[idx].data_idx;
    dmap_freelist_push(d, data_index);
    d->table[idx].data_idx = DMAP_DELETED;

    if(!d->options.user_managed_keys && key_size > 8){ // dmap copies keys
        free(d->table[idx].ptr);
        d->table[idx].ptr = NULL;
    }
    else if(d->options.user_managed_keys && d->options.free_key_fn){ // user supplied free_key
        d->options.free_key_fn(d->table[idx].ptr);
        d->table[idx].ptr = NULL;
    }
    else { // user managed 
        d->table[idx].kstr = NULL;
    }
    d->len -= 1; 
    return data_index;
}
// len of the data array, including invalid table. For iterating
s32 dmap__range(DmapHdr *d){ 
    return d ? d->len + d->free_list->len : 0; 
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


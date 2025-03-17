# dmap

### Dmap is a flexible, lightweight, zero-friction dynamic hashmap implementation in C, designed to be user-friendly without sacrificing performance.

---

## 🚀 Super Easy – Zero Setup Required

```c
// Declare a dynamic hashmap of any type, `int` in this case.
int *my_dmap = NULL;

// Insert the value 33. You can use any basic data types (ints,floats, etc) or C-strings.
int key = 13;
dmap_insert(my_dmap, &key, 33);

// Retrieve the value.
size_t idx = dmap_get_idx(my_dmap, &key);

printf("result: %d\n", my_dmap[idx]); // output: result: 33

```

## 🔧 Features
- **No boilerplate** – Zero setup required.
- **Generic typing** – Supports multiple key and value types.  
- **Dynamic memory** – Grows as needed.  
- **Cross-platform** – Works on Linux, macOS, and Windows.  
- **Good performance** – Competitive with leading hashmap implementations.  
- **Supports compound literals** – Insert structs inline.

**Supported platforms:** Linux, macOS (untested), and Windows. **64-bit only.**  

---

## ⚡ Performance
**Dmap is designed for simplicity and ease of use, while still outperforming widely-used hashmaps like `uthash` and `std::unordered_map`.**  

- **Stores values directly in a dynamic array** → No pointer indirection.  
- **Better cache locality** → Faster lookups and iteration.  
- **30% to 40% faster than `uthash`** in benchmarks like [UDB3](https://github.com/attractivechaos/udb3).  
- **Supports index-based lookups** → Treat the hashmap like an array.  

---

## 🔍 Hash Collisions
- Keys are compared directly.
- The library stores **raw key bytes** for 1, 2, 4, and 8-byte keys, and assumes they are basic data types like integers and floating-point numbers that can be compared using `memcmp()`.
- String keys are copied, allocated with `malloc()`, and freed on `dmap_delete` or `dmap_free`.
- Future versions will support user-managed string lifetimes and user-supplied destructors.
- Struct keys are not currently supported. See the TODO list.

---

## ⚠️ Error Handling
- By default, memory allocation failures trigger an error and `exit()`.  
- A custom error handler can be set using `dmap_set_error_handler` to handle allocation failures gracefully.  

---

## 📦 Memory Management
- Dmap allows **storing complex structs values directly** in the hashmap
- **Compound literals** allow inline struct initialization.

### Example: Using String Keys with Struct Values

```c
#include <stdio.h>
#include <string.h>
#include "dmap.h"  // Your dmap header

// Define a struct to store directly in the hashmap
typedef struct {
    int id;
    int age;
    float balance;
} UserProfile;

int main() {
    UserProfile *user_map = NULL;  // Declare a dynamic hashmap

    // Insert user profiles with email addresses as keys
    const char *email1 = "alice@example.com";
    const char *email2 = "bob@example.com";

    UserProfile alice = {1, 28, 1050.75};
    dmap_kstr_insert(user_map, email1, strlen(email1), alice);
    // or use compound literals
    dmap_kstr_insert(user_map, email2, strlen(email2), (UserProfile){2, 35, 893.42}); 

    // Retrieve user profiles
    UserProfile *alice_profile = dmap_kstr_get(user_map, email1, strlen(email1));
    UserProfile *bob_profile = dmap_kstr_get(user_map, email2, strlen(email2));

    if (alice_profile)
        printf("Alice: ID=%d, Age=%d, Balance=%.2f\n",
               alice_profile->id, alice_profile->age, alice_profile->balance);

    if (bob_profile)
        printf("Bob: ID=%d, Age=%d, Balance=%.2f\n",
               bob_profile->id, bob_profile->age, bob_profile->balance);

    return 0;
}
```

## 🔄 Efficient Storage & Iteration
Unlike traditional hashmaps that store pointers to data, **Dmap stores values directly in a dynamic array**, allowing **efficient iteration**.  

- **Direct array-based storage** → Iterate over stored values efficiently.  
- **Bulk processing** → Ideal for SIMD operations, filtering, or batch updates.  
- **Index-based access** → Retrieve values by position using `dmap_get_idx()`.  

---

## ⚠️ Limitations  
- **64-bit systems only**
- **Struct keys** are not currently supported because `memcmp()` does not account for struct padding. Future versions will allow custom hash and comparison functions for struct keys. See the TODO list.
- **Macro arguments (d, k) may be evaluated multiple times** – Avoid using expressions with side effects.  
- **Key sizes are compared at runtime** – However, users must ensure consistency in key types.  
- **Untested on macOS** – Compatibility is not guaranteed.  
- **C++ support** is a work in progress.
- **dmap_get** uses `typeof()` (or `decltype()` in C++) for type safety, but falls back to a `void*` return in environments where `typeof()` is unavailable.  
- **Pointer Validity**: The pointer returned by `dmap_get` is only valid as long as the dmap is not modified. Insertions or reallocations may invalidate the pointer. Use an index via `dmap_get_idx` if this is an issue.

---

## TODO:
- Add a full test suite (unit + stress tests)
- Support struct keys with user-defined hash & comparison functions
- Allow user-managed string and struct keys (opt-out of copying)
- Add support for user-defined key destructors for string and struct keys
- Expand documentation 

## Full Example: Dmap Usage

```c
#include "dmap.h"
#include <stdio.h>
#include <string.h>

int main() {

    // Declare a dynamic hashmap (can store any type)
    int *my_dmap = NULL;

    // Optional: Initialize with a custom allocator (requires contiguous memory)
    // dmap_init(my_dmap, 1024 * 1024, v_alloc_realloc);

    // Insert values into the hashmap using integer keys
    int key_1 = 1;
    int key_2 = 2;
    dmap_insert(my_dmap, &key_1, 33);   
    dmap_insert(my_dmap, &key_2, 13);   

    // Retrieve a *value using an integer key
    int *value = dmap_get(my_dmap, &key_1);
    if (value) {
        printf("Value for key_1 1: %d\n", *value);  
    }
    // ================================
    // declare a hashmap that uses strings as keys
    int *my_kstr_dmap = NULL;
    // Use a C-string as key
    char *str_key = "my_key";

    // Optional: Initialize the key-string hashmap w/ custom allocator
    // dmap_kstr_init(my_kstr_dmap, 1024 * 1024, v_alloc_realloc); 

    // Insert a value using a string key
    dmap_kstr_insert(my_kstr_dmap, str_key, strlen(str_key), 33); // string keys need length param

    // Retrieve a *value using a string key
    value = dmap_kstr_get(my_kstr_dmap, str_key, strlen(str_key));
    if (value) {
        printf("Value for key 'str_key': %d\n", *value);  
    }
    // ================================
    // Get an index
    // Retrieve an index to a value using an integer key - treat it like an array
    size_t idx = dmap_get_idx(my_dmap, &key_1);
    if (idx != DMAP_INVALID) {
        printf("Index based result for key_1: %d\n", my_dmap[idx]);  
    }

    // ================================
    // Deletions

    // Delete a key from the hashmap
    size_t deleted_index = dmap_delete(my_dmap, &key_2);
    if (deleted_index != DMAP_INVALID) {
        printf("Deleted key_2, data index: %zu\n", deleted_index);  
        // Mark the deleted entry as invalid for safe iteration
        // Here, we use -1 to represent an invalid state.
        my_dmap[deleted_index] = -1;
    }
    // Check if a key exists after deletion
    value = dmap_get(my_dmap, &key_2);
    if (!value) {
        printf("key_2 no longer exists in the hashmap.\n");  
    }

    // ================================
    // Iterate over the hashmap data - treat it like an array

    // Get the range of valid data indices (including deleted slots)
    size_t range = dmap_range(my_dmap);
    printf("hashmap data array range: %zu\n", range);  

    // Iterate over the data array (including deleted slots)
    for (size_t i = 0; i < range; i++) {
        if (my_dmap[i] != -1) {  // Skip invalid/deleted entries
            printf("Data at index %zu: %d\n", i, my_dmap[i]);
        }
    }

    // Free the hashmap and set the pointer to NULL
    dmap_free(my_dmap);

    return 0;
}
```

## License

[MIT License](LICENSE)

### Credits & Inspiration  
*dmap* is inspired by Per Vognsen's dynamic array implementation on his *Bitwise* series, which was itself based on Sean Barrett's [`stb_ds`](https://github.com/nothings/stb/blob/master/stb_ds.h) library. 

[Per Vognsen's Bitwise series on YouTube](https://www.youtube.com/pervognsen).
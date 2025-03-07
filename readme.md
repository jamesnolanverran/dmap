# dmap

### **Dmap** is a lightweight, zero-friction dynamic hashmap implementation in C, designed to be user friendly without sacrificing performance.

## 🚀 Super Easy – Zero Setup Required

```c
// Declare a dynamic hashmap of any type, `int` in this case.
int *my_dmap = NULL;

// Insert the value 33. You can use any key type.
int key = 13;
dmap_insert(my_dmap, &key, 33);

// Retrieve the value.
int *value = dmap_get(my_dmap, &key);

printf("result: %d\n", *value); // output: result: 33

```
- **No manual setup** → Just declare and use.  
- **Dynamic value storage** → Supports any value type without explicit casting.  
- **Flexible key types** → Works with integers, strings, and more.  
- **Automatic memory management** → Grows dynamically as needed.  

## Performance
- Dmap is designed for simplicity and ease of use, while still outperforming widely-used hashmaps like uthash and std::unordered_map.
- [UDB3](https://github.com/attractivechaos/udb3) benchmarks and my own testing show that **dmap is up to 30% to 40% faster than uthash**.

## Features

- dynamic typing
- dynamic memory
- cross-platform
- good performance

Supported platforms: **Linux, macOS, and Windows**. 64-bit only. (Note: macOS support is untested.)

---

## Hash Collisions
- The library stores **raw key bytes** for 1, 2, 4 and 8 bytes keys. If a hash collision occurs, keys are compared directly.  
- For string and custom struct keys, **two 64-bit hashes** are stored instead of the key. While hash collisions are extremely rare (less than 1 in 10¹⁸ for a trillion keys), they are still possible. Future versions will make improvements here.

## Error Handling
- By default, memory allocation failures trigger an error and exit().
- A custom error handler can be set using `dmap_set_error_handler` to handle allocation failures gracefully.

## Memory Management
User can specify allocator using an init function.

## Limitations

- Currently 64-bit Only
- Currently Not Thread-Safe
- Untested on macOS
- Key sizes are compared at runtime. It is up to users to ensure key types are consistent.

## Full Example: Dmap Usage

```c
#include "dmap.h"
#include <stdio.h>
#include <string.h>

int main() {

    // Declare a dynamic hashmap (can store any type)
    int *my_dmap = NULL;

    // Optional: Initialize the hashmap with an initial capacity
    dmap_init(my_dmap, 1024 * 1024, ALLOC_MALLOC); 
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

    // Optional: Initialize the key-string hashmap
    dmap_kstr_init(my_kstr_dmap, 1024 * 1024, ALLOC_MALLOC); 
    // Insert a value using a string key
    dmap_kstr_insert(my_kstr_dmap, str_key, 33, strlen(str_key)); // string keys need length param

    // Retrieve a value using a string key
    value = dmap_kstr_get(my_kstr_dmap, str_key, strlen(str_key));
    if (value) {
        printf("Value for key 'str_key': %d\n", *value);  
    }
    // ================================
    // Get an index
    // Retrieve an index to a value using an integer key
    size_t idx = dmap_get_idx(my_dmap, &key_1);
    if (idx != DMAP_INVALID) {
        printf("Index based result for key_1: %d\n", my_dmap[idx]);  
    }

    // ================================
    // Deletions

    // Delete a key from the hashmap
    size_t deleted_index = dmap_delete(my_dmap, &key_2);
    if (deleted_index != DMAP_EMPTY) {
        printf("Deleted key_2, data index: %zu\n", deleted_index);  
        // Mark the deleted entry as invalid for safe iteration
        // If we intend to iterate over the data array directly, we need to indicate that deleted data is invalid.
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
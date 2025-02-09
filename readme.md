# dmap

### **Dmap** is a lightweight, zero-friction dynamic hashmap implementation in C, designed to be user friendly without sacrificing performance.

## Includes:

- **dmap** – A dynamic hashmap implementation with dynamic typing.
- **darr** – A dynamic array implementation with dynamic typing.
- **v_alloc** – A cross-platform reserve/commit-style arena allocator.

Supported platforms: **Linux, macOS, and Windows**. 64-bit only. (Note: macOS support is untested.)

---

## Features

- dynamic typing
- dynamic memory
- cross-platform
- fast
- stable pointers

---

## Performance
- The library is designed for **ease of use** without sacrificing performance.
- Benchmarked against uthash, **dmap** performs 2-3× faster for insertions and uses 2-7× less memory.

## Memory Management

The dmap and darr libs support two memory management models:

1. **Malloc/Realloc Model**:  
   - Uses traditional `malloc` and `realloc` for memory allocation.
   - This is the default for `darr` and `dmap`.

2. **Reserve/Commit Model**:  
   - Uses `VirtualAlloc` or `mmap` to reserve and commit memory, providing stable pointers and avoiding reallocations.

An optional initialization function allows switching between these models.

---

## Example Dmap Usage

```c
#include "dmap.h"
#include <stdio.h>
#include <string.h>

int main() {

    // Declare a dynamic hashmap (can store any type)
    int *my_dmap = NULL;

    // Optional: Initialize the hashmap with custom settings
    // dmap_init(my_dmap, 256, ALLOC_MALLOC);

    // Insert values into the hashmap using integer keys
    int key_1 = 1;
    int key_2 = 2;
    dmap_insert(my_dmap, &key_1, 42);   
    dmap_insert(my_dmap, &key_2, 13);   

    // Retrieve a value using an integer key_1
    int *value = dmap_get(my_dmap, &key_1);
    if (value) {
        printf("Value for key_1 1: %d\n", *value);  
    }

    // Use a C-string as key
    char *str_key = "my_key";

    // Insert a value using a string key
    dmap_kstr_insert(my_dmap, str_key, 33, strlen(str_key)); // string keys need length param

    // Retrieve a value using a string key
    value = dmap_kstr_get(my_dmap, str_key, strlen(str_key));
    if (value) {
        printf("Value for key 'str_key': %d\n", *value);  
    }

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

    // Iterate over the hashmap
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
## Design Considerations

### Hash Collisions
- The library uses **128-bit Murmur3 hashes** and does not store original keys. As a result, it does not handle hash collisions internally.
- The probability of a collision with a 128-bit hash is extremely low (e.g., less than 1 in 10^18 for 1 trillion keys). For most use cases, this is negligible.
- If collision handling is required, users can bundle the hash key with the value in a struct and implement their own collision resolution logic.

### Error Handling
- By default, memory allocation failures trigger an error and exit().
- A custom error handler can be set using `dmap_set_error_handler` to handle allocation failures gracefully.

---

## Limitations

- **Currently 64-bit Only**
- **Currently Not Thread-Safe**
- **Untested on macOS**
- **No Built-in Collision Handling** (see above)

---

## Example V_Alloc Usage

```c
#include "dmap.h"
#include <stdio.h>

int main() {
    // Initialize virtual memory allocator
    AllocInfo alloc_info = {0};
    size_t reserve_size = 1024 * 1024; // 1MB
    if (!v_alloc_reserve(&alloc_info, reserve_size)) {
        // handle error
    }

    // Allocate a string using the bump allocator
    size_t str_len = 32;
    char *str = v_alloc_committ(&alloc_info, str_len + 1);
    if (!str) {
        // handle error
    }
    snprintf(str, str_len + 1, "Hello, V_Alloc!");
    printf("%s\n", str);

    // Reset allocator (memory can be reused)
    v_alloc_reset(&alloc_info);

    // Allocate another object
    int *arr = v_alloc_committ(&alloc_info, 10 * sizeof(int));
    if (!arr) {
        // handle error
    }
    arr[0] = 42;
    printf("First value: %d\n", arr[0]);

    // Free virtual memory
    if (!v_alloc_free(&alloc_info)) {
        // handle error
    }

    return 0;
}
```

---

## Example Darr Usage

```c
#include "dmap.h"
#include <stdio.h>

int main() {

    // Declare a dynamic array (can store any type)
    int *my_array = NULL;

    // OPTIONAL: initialize the array with custom settings: initial capacity 512
    // darr_init(my_array, 512, ALLOC_MALLOC);

    // Push some elements into the array
    darr_push(my_array, 10);
    darr_push(my_array, 20);
    darr_push(my_array, 30);

    // Print the array contents
    printf("Array after pushing 10, 20, 30:\n");
    for (size_t i = 0; i < darr_len(my_array); i++) {
        printf("my_array[%zu] = %d\n", i, my_array[i]);
    }

    // Pop the last element
    int last_element = darr_pop(my_array);
    printf("Popped element: %d\n", last_element);  

    // Peek at the new last element
    int new_last_element = darr_peek(my_array);
    printf("New last element: %d\n", new_last_element);  

    // Clear the array for reuse (reset length to 0)
    darr_clear(my_array);
    printf("Array cleared. Length: %zu\n", darr_len(my_array));  

    // Free the array and set the pointer to NULL
    darr_free(my_array);
    printf("Array freed.\n");

    return 0;
}
```

## License

[MIT License](LICENSE)

## Credits & Inspiration
`darr` is adapted from **Per Vognsen's Bitwise series**. His work inspired me to build a similar approach for hashmaps with `dmap`.
[Per Vognsen on YouTube](https://www.youtube.com/pervognsen).
```C
    // Use a C-string as a key_1
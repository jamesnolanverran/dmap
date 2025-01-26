# hmap

**hmap** is a lightweight C library for dynamic memory management, designed for simplicity and ease of use. It includes:

- **hmap** – A hashmap implementation with dynamic typing.
- **darr** – A dynamic array implementation with dynamic typing.
- **v_alloc** – A cross-platform reserve/commit-style arena allocator.

Supported platforms: **Linux, macOS, and Windows**. 64-bit only. (Note: macOS support is untested.)

---

## Features

- Simple API with minimal setup required.
- Cross-platform compatibility.
- Lightweight and efficient memory management.

---

## Memory Management

The library supports two memory management models:

1. **Reserve/Commit Model**:  
   - Uses `VirtualAlloc` (or equivalent) to reserve and commit memory, providing stable pointers and avoiding reallocations.
   - This is the default for `hmap`.

2. **Malloc/Realloc Model**:  
   - Uses traditional `malloc` and `realloc` for memory allocation.
   - This is the default for `darr`.

An optional initialization function allows switching between these models.

---

## Example Hmap Usage

```c
#include "hmap.h"
#include <stdio.h>
#include <string.h>

int main() {
    // ============================================
    // Section 1: Initialize the hashmap
    // ============================================
    // Declare a dynamic hashmap (can store any type)
    int *my_hmap = NULL;

    // Initialize the hashmap with default settings
    hmap_init(my_hmap, 0, sizeof(int), ALLOC_MALLOC);

    // ============================================
    // Section 2: Insert and retrieve integer keys
    // ============================================
    // Insert values into the hashmap using integer keys
    hmap_insert(my_hmap, 1, 42);   // Key = 1, Value = 42
    hmap_insert(my_hmap, 2, 99);   // Key = 2, Value = 99

    // Retrieve a value using an integer key
    int *value = hmap_get(my_hmap, 1);
    if (value) {
        printf("Value for key 1: %d\n", *value);  // Output: Value for key 1: 42
    }

    // ============================================
    // Section 3: Insert and retrieve string keys
    // ============================================
    // Use a C-string as a key
    char *str_key = "my_key";
    size_t key_len = strlen(str_key);

    // Insert a value using a string key
    hmap_kstr_insert(my_hmap, str_key, 33, key_len);

    // Retrieve a value using a string key
    value = hmap_kstr_get(my_hmap, str_key, key_len);
    if (value) {
        printf("Value for key 'my_key': %d\n", *value);  // Output: Value for key 'my_key': 33
    }

    // ============================================
    // Section 4: Delete a key and check existence
    // ============================================
    // Delete a key from the hashmap
    size_t deleted_index = hmap_delete(my_hmap, 2);
        // ============================================
    // Section 4: Delete a key and mark it as invalid
    // ============================================
    // Delete a key from the hashmap
    size_t deleted_index = hmap_delete(my_hmap, 2);
    if (deleted_index != HMAP_EMPTY) {
        printf("Deleted key 2, data index: %zu\n", deleted_index);  // Output: Deleted key 2, data index: 1

        // Mark the deleted entry as invalid for safe iteration
        // If we intend to iterate over the data array directly, we need to indicate that deleted data is invalid.
        // Here, we use -1 to represent an invalid state.
        my_hmap[deleted_index] = -1;
    }

    // Check if a key exists after deletion
    value = hmap_get(my_hmap, 2);
    if (!value) {
        printf("Key 2 no longer exists in the hashmap.\n");  // Output: Key 2 no longer exists in the hashmap.
    }

    // ============================================
    // Section 5: Iterate over the hashmap
    // ============================================
    // Get the range of valid data indices (including deleted slots)
    size_t range = hmap_range(my_hmap);
    printf("Hashmap data array range: %zu\n", range);  // Output: Hashmap data array range: 2

        // Iterate over the data array (including deleted slots)
    for (size_t i = 0; i < range; i++) {
        if (my_hmap[i] != -1) {  // Skip invalid/deleted entries
            printf("Data at index %zu: %d\n", i, my_hmap[i]);
        }
    }
    // Output:
    // Data at index 0: 42
    // Data at index 2: 33

    // ============================================
    // Section 6: Clean up
    // ============================================
    // Free the hashmap and set the pointer to NULL
    hmap_free(my_hmap);

    return 0;
}
```
## Design Considerations

### Hash Collisions
- The library uses **128-bit Murmur3 hashes** and does not store original keys. As a result, it does not handle hash collisions internally.
- The probability of a collision with a 128-bit hash is extremely low (e.g., less than 1 in 10^18 for 1 trillion keys). For most use cases, this is negligible.
- If collision handling is required, users can bundle the hash key with the value in a struct and implement their own collision resolution logic.

### Performance
- The library is designed for **ease of use and prototyping**. It prioritizes simplicity and flexibility over raw performance.
- For performance-critical applications, users may consider swapping out `hmap` for a more specialized or optimized data structure once the design is finalized.

### Error Handling
- By default, memory allocation failures trigger a call to `perror` followed by `exit`.
- A custom error handler can be set using `hmap_set_error_handler` to handle allocation failures gracefully.

---

## Limitations

- **Currently 64-bit Only**: The library is currently designed and tested for 64-bit systems. It has not been tested on 32-bit platforms.
- **Currently Not Thread-Safe**: The library is not thread-safe. Concurrent access to the same `hmap` or `darr` instance from multiple threads may lead to race conditions.
- **Untested on macOS**: While the library is expected to work on macOS, it has not been thoroughly tested on this platform.
- **No Built-in Collision Handling**: The library does not handle hash collisions internally. Users must implement their own collision resolution if required.

---

## Example V_Alloc Usage

```c
#include "hmap.h"
#include <stdio.h>

int main() {
    // ============================================
    // Section 1: Initialize the virtual memory allocator
    // ============================================
    AllocInfo alloc_info = {0};

    // Reserve 1MB of virtual memory
    size_t reserve_size = 1024 * 1024; // 1MB
    if (!v_alloc_reserve(&alloc_info, reserve_size)) {
        printf("Failed to reserve virtual memory.\n");
        return 1;
    }

    // ============================================
    // Section 2: Commit memory and use it
    // ============================================
    // Commit 64KB of memory
    size_t commit_size = 64 * 1024; // 64KB
    void *committed_memory = v_alloc_committ(&alloc_info, commit_size);
    if (!committed_memory) {
        printf("Failed to commit memory.\n");
        return 1;
    }

    // Use the committed memory (e.g., store some data)
    int *data = (int *)committed_memory;
    data[0] = 42;
    data[1] = 99;
    printf("Stored values: %d, %d\n", data[0], data[1]);  // Output: Stored values: 42, 99

    // ============================================
    // Section 3: Reset the allocator
    // ============================================
    // Reset the allocator to reuse the reserved memory
    v_alloc_reset(&alloc_info);
    printf("Allocator reset. Memory can be reused.\n");

    // ============================================
    // Section 4: Free the virtual memory
    // ============================================
    // Free the reserved virtual memory
    if (!v_alloc_free(&alloc_info)) {
        printf("Failed to free virtual memory.\n");
        return 1;
    }
    printf("Freed virtual memory.\n");

    return 0;
}
```

---

## Example Darr Usage

```c
#include "hmap.h"
#include <stdio.h>

int main() {
    // ============================================
    // Section 1: Initialize the dynamic array
    // ============================================
    // Declare a dynamic array (can store any type)
    int *my_array = NULL;

    // OPTIONAL:
    // initialize the array with custom settings
    darr_init(my_array, 512, ALLOC_VIRTUAL);

    // ============================================
    // Section 2: Push elements into the array
    // ============================================
    // Push some elements into the array
    darr_push(my_array, 10);
    darr_push(my_array, 20);
    darr_push(my_array, 30);

    // Print the array contents
    printf("Array after pushing 10, 20, 30:\n");
    for (size_t i = 0; i < darr_len(my_array); i++) {
        printf("my_array[%zu] = %d\n", i, my_array[i]);
    }
    // Output:
    // my_array[0] = 10
    // my_array[1] = 20
    // my_array[2] = 30

    // ============================================
    // Section 3: Pop and peek elements
    // ============================================
    // Pop the last element
    int last_element = darr_pop(my_array);
    printf("Popped element: %d\n", last_element);  // Output: Popped element: 30

    // Peek at the new last element
    int new_last_element = darr_peek(my_array);
    printf("New last element: %d\n", new_last_element);  // Output: New last element: 20

    // ============================================
    // Section 4: Clear the array
    // ============================================
    // Clear the array for reuse (reset length to 0)
    darr_clear(my_array);
    printf("Array cleared. Length: %zu\n", darr_len(my_array));  // Output: Array cleared. Length: 0

    // ============================================
    // Section 5: Free the array
    // ============================================
    // Free the array and set the pointer to NULL
    darr_free(my_array);
    printf("Array freed.\n");

    return 0;
}
```

## License

[MIT License](LICENSE)
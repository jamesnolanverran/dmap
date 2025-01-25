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

## Example Usage

```c
#include "hmap.h"
#include <stdio.h>

int main() {
    // Declare a dynamic hashmap (can store any type)
    int *my_hmap = NULL;

    // Insert values into the hashmap
    hmap_insert(my_hmap, 1, 42);   // Key = 1, Value = 42
    hmap_insert(my_hmap, 2, 99);   // Key = 2, Value = 99

    // Retrieve a value
    int *value = hmap_get(my_hmap, 1);
    if (value) {
        printf("Value for key 1: %d\n", *value);
    }

    // Use a C-string as a key
    char *str_key = "my_key";
    size_t key_len = strlen(str_key);

    hmap_kstr_insert(my_hmap, str_key, 33, key_len);
    value = hmap_kstr_get(my_hmap, str_key, key_len);

    if (value) {
        printf("Value for key 'my_key': %d\n", *value);
    }

    // Clean up
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

- **64-bit Only**: The library is designed for 64-bit systems and has not been tested on 32-bit platforms.
- **Untested on macOS**: While the library is expected to work on macOS, it has not been thoroughly tested on this platform.
- **No Built-in Collision Handling**: Users must implement their own collision resolution if required.

---

## License

[MIT License](LICENSE)
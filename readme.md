# hmap

**hmap** is a lightweight **low-friction** C library for dynamic memory management. It includes:

- **hmap** – A stretchy buffer-style hashmap with dynamic typing.
- **darr** – A stretchy buffer-style dynamic array with dynamic typing.
- **v_alloc** – A cross-platform reserve/commit-style arena allocator.

Support for **Linux, macOS, and Windows**. (untested on mac)

## Features

- Simple API, minimal setup.
- Cross-platform compatibility.
- Low-friction, lightweight and efficient.

## Memory Management

By using a reserve/commit memory model with `VirtualAlloc` (or equivalent), `darr` and `hmap` can provide stable pointers, avoiding reallocations that typically require working with indices instead. An optional init function allows switching between traditional `malloc`/`realloc` or the reserve/commit approach, giving flexibility based on the use case.

hmap uses reserve/commit by default.
darr uses malloc/realloc by default.

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

    // use a c-string as a key
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

## License

MIT
#ifndef STORE_H
#define STORE_H

#include <stddef.h> // For size_t
#include <stdint.h> // For uint32_t

// --- Hash Map Entry ---
typedef struct Entry {
    char* key;
    char* value;
    struct Entry* next; // For collision resolution (separate chaining)
} Entry;

// --- Hash Map Structure ---
// Using a simple fixed-size array of linked lists for now.
#define HASH_MAP_SIZE 1024 // A power of 2 is good for simple modulo hashing

typedef struct {
    Entry* buckets[HASH_MAP_SIZE];
} HashMap;

// --- Public Functions ---
// Initialize the hash map
void store_init(HashMap* map);

// Set a key-value pair. Returns 1 on success, 0 on memory allocation failure.
int store_set(HashMap* map, const char* key, const char* value);

// Get the value associated with a key. Returns a dynamically allocated string
// or NULL if the key is not found. The caller is responsible for freeing the string.
char* store_get(HashMap* map, const char* key);

// Delete a key-value pair. Returns 1 if key was found and deleted, 0 otherwise.
int store_del(HashMap* map, const char* key);

// Clean up the hash map, freeing all allocated memory.
void store_free(HashMap* map);

#endif // STORE_H
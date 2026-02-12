#ifndef STORE_H
#define STORE_H

#include <stddef.h> // For size_t
#include <stdint.h> // For uint32_t

// --- Object Types ---
typedef enum {
    OBJ_STRING,
    OBJ_HASH
} ObjType;

// --- Hash Map Entry ---
typedef struct Entry {
    char* key;
    void* value; // Can be char* for string or HashMap* for hash
    ObjType type;
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

// Set a field-value pair within a hash key. Returns 1 on success, 0 on failure.
int store_hset(HashMap* map, const char* key, const char* field, const char* value);

// Get the value associated with a field in a hash key. Returns a dynamically allocated string
// or NULL if the key or field is not found, or if the key is not a hash.
// The caller is responsible for freeing the string.
char* store_hget(HashMap* map, const char* key, const char* field);

// Get the number of fields in a hash key. Returns -1 if key is not found or not a hash,
// otherwise returns the number of fields.
int store_hlen(HashMap* map, const char* key);

#endif // STORE_H
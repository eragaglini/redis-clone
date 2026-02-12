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
int store_delete_entry_from_map(HashMap* map, const char* key);

// Clean up the hash map, freeing all allocated memory.
void store_free(HashMap* map);

// Top-level Key Commands
// Delete one or more keys. Returns the number of keys that were removed.
int store_del(HashMap* map, const char** keys, size_t num_keys);

// Check if a key exists. Returns 1 if the key exists, 0 otherwise.
int store_exists(HashMap* map, const char* key);

// Get the type of a key. Returns OBJ_STRING, OBJ_HASH, or a special value (e.g., -1 cast to ObjType) if the key does not exist.
ObjType store_type(HashMap* map, const char* key);

// Set a field-value pair within a hash key. Returns 1 on success, 0 on failure.
int store_hset(HashMap* map, const char* key, const char* field, const char* value);

// Get the value associated with a field in a hash key. Returns a dynamically allocated string
// or NULL if the key or field is not found, or if the key is not a hash.
// The caller is responsible for freeing the string.
char* store_hget(HashMap* map, const char* key, const char* field);

// Get the number of fields in a hash key. Returns -1 if key is not found or not a hash,
// otherwise returns the number of fields.
int store_hlen(HashMap* map, const char* key);

// Delete a field-value pair from a hash key. Returns 1 if the field was present and deleted,
// 0 if the field or the key didn't exist or if the key is not a hash.
int store_hdel(HashMap* map, const char* key, const char* field);

// Retrieves all fields and values for a given hash key.
// out_results: Pointer to an array of char* where results will be stored (field1, value1, field2, value2, ...).
//              The caller is responsible for freeing each string and the array itself.
// out_count: Pointer to a size_t to store the total number of strings in out_results (2 * number of fields).
// Returns 0 on success, -1 if the key is not found or is not a hash.
int store_hgetall(HashMap* map, const char* key, char*** out_results, size_t* out_count);

#endif // STORE_H
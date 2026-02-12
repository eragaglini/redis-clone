#include "store.h"
#include <stdlib.h> // For malloc, free
#include <string.h> // For strdup, strcmp
#include <stdio.h>  // For snprintf in debug

// --- Private Helper Functions ---

// Simple Jenkins hash function (one-at-a-time hash)
// from https://en.wikipedia.org/wiki/Jenkins_hash_function
static uint32_t jenkins_hash(const char* key) {
    size_t i = 0;
    uint32_t hash = 0;
    while (key[i] != '\0') {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
        i++;
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

static uint32_t get_bucket_index(const char* key) {
    return jenkins_hash(key) % HASH_MAP_SIZE;
}

// --- Public Functions Implementation ---

void store_init(HashMap* map) {
    if (!map) return;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        map->buckets[i] = NULL;
    }
}

int store_set(HashMap* map, const char* key, const char* value) {
    if (!map || !key || !value) return -1; // -1 for error

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* prev = NULL;

    // Check if key already exists
    while (current) {
        if (strcmp(current->key, key) == 0) {
            // If the existing entry is not a string, return error
            if (current->type != OBJ_STRING) {
                // Cannot overwrite a hash with a string
                return -1; // -1 for type mismatch error
            }
            // Free old string value
            free((char*)current->value);
            // Update with new string value
            current->value = strdup(value);
            if (!current->value) return -1; // OOM
            return 0; // Key updated
        }
        prev = current;
        current = current->next;
    }

    // Key does not exist, create new entry
    Entry* new_entry = (Entry*)malloc(sizeof(Entry));
    if (!new_entry) return -1; // OOM

    new_entry->key = strdup(key);
    new_entry->value = strdup(value); // Value is a string (char*)
    new_entry->type = OBJ_STRING;
    new_entry->next = NULL;

    if (!new_entry->key || !new_entry->value) {
        if (new_entry->key) free(new_entry->key);
        if (new_entry->value) free(new_entry->value); // Only free if allocated
        free(new_entry);
        return -1; // OOM
    }

    // Add to the beginning of the bucket's linked list
    new_entry->next = map->buckets[index];
    map->buckets[index] = new_entry;

    return 1; // New key added
}

char* store_get(HashMap* map, const char* key) {
    if (!map || !key) return NULL;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    while (current) {
        if (strcmp(current->key, key) == 0) {
            if (current->type == OBJ_STRING) {
                return strdup((char*)current->value); // Return a copy, caller frees
            } else {
                // Key found, but it's not a string type
                return NULL;
            }
        }
        current = current->next;
    }
    return NULL; // Key not found
}

int store_delete_entry_from_map(HashMap* map, const char* key) {
    if (!map || !key) return 0;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* prev = NULL;

    while (current) {
        if (strcmp(current->key, key) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                map->buckets[index] = current->next;
            }
            free(current->key);
            // Free value based on its type
            if (current->type == OBJ_STRING) {
                free((char*)current->value);
            } else if (current->type == OBJ_HASH) {
                // Recursively free the inner hash map
                store_free((HashMap*)current->value);
                free((HashMap*)current->value); // Free the HashMap struct itself
            }
            free(current);
            return 1; // Deleted
        }
        prev = current;
        current = current->next;
    }
    return 0; // Key not found
}

// Helper function to count entries in a HashMap
static int count_hash_entries(HashMap* map) {
    if (!map) return 0;
    int count = 0;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        Entry* current = map->buckets[i];
        while (current) {
            count++;
            current = current->next;
        }
    }
    return count;
}

int store_hset(HashMap* map, const char* key, const char* field, const char* value) {
    if (!map || !key || !field || !value) return -1; // -1 for error

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (strcmp(current->key, key) == 0) {
            main_entry = current;
            break;
        }
        current = current->next;
    }

    if (!main_entry) {
        // Main key does not exist, create a new hash map for it
        HashMap* new_hash_map = (HashMap*)malloc(sizeof(HashMap));
        if (!new_hash_map) return -1; // OOM
        store_init(new_hash_map);

        // Create a new entry for the main map
        Entry* new_main_entry = (Entry*)malloc(sizeof(Entry));
        if (!new_main_entry) {
            free(new_hash_map);
            return -1; // OOM
        }
        new_main_entry->key = strdup(key);
        new_main_entry->value = new_hash_map; // Value is the inner HashMap*
        new_main_entry->type = OBJ_HASH;
        new_main_entry->next = NULL;

        if (!new_main_entry->key) {
            free(new_main_entry->key);
            free(new_main_entry);
            free(new_hash_map);
            return -1; // OOM
        }

        // Add to the beginning of the bucket's linked list in the main map
        new_main_entry->next = map->buckets[index];
        map->buckets[index] = new_main_entry;

        // Set the field in the newly created inner hash map
        int set_result = store_set(new_hash_map, field, value);
        if (set_result == 1) return 1; // New field added
        // If set_result is 0 (updated) or -1 (error), it means the field was somehow
        // already there in the fresh map or OOM. This case should ideally not happen
        // if new_hash_map is truly fresh. Return -1 for safety in unexpected cases.
        return (set_result == 0) ? 0 : -1;

    } else {
        // Main key exists
        if (main_entry->type != OBJ_HASH) {
            // Key exists but is not a hash, cannot perform HSET
            return -1; // Type mismatch error
        }
        // Key is a hash, perform HSET on the inner hash map
        return store_set((HashMap*)main_entry->value, field, value);
    }
}

char* store_hget(HashMap* map, const char* key, const char* field) {
    if (!map || !key || !field) return NULL;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    // Find the main key entry
    while (current) {
        if (strcmp(current->key, key) == 0) {
            if (current->type == OBJ_HASH) {
                // Key is a hash, perform HGET on the inner hash map
                return store_get((HashMap*)current->value, field);
            } else {
                // Key exists but is not a hash
                return NULL;
            }
        }
        current = current->next;
    }
    return NULL; // Key not found
}

int store_hlen(HashMap* map, const char* key) {
    if (!map || !key) return 0; // Return 0 for invalid input, consistent with non-existent key

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    // Find the main key entry
    while (current) {
        if (strcmp(current->key, key) == 0) {
            if (current->type == OBJ_HASH) {
                // Key is a hash, return the count of entries in the inner hash map
                return count_hash_entries((HashMap*)current->value);
            } else {
                // Key exists but is not a hash, return 0 (as per Redis HLEN behavior)
                return 0;
            }
        }
        current = current->next;
    }
    return 0; // Key not found, return 0 as per Redis HLEN behavior
}

int store_hdel(HashMap* map, const char* key, const char** fields, size_t num_fields) {
    if (!map || !key || !fields || num_fields == 0) return 0; // Invalid input, return 0 as per Redis HDEL if no fields or invalid map/key

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (strcmp(current->key, key) == 0) {
            main_entry = current;
            break;
        }
        current = current->next;
    }

    if (!main_entry) {
        // Key not found
        return 0;
    }

    if (main_entry->type != OBJ_HASH) {
        // Key is not a hash
        return -1; // Indicate wrong type
    }

    HashMap* inner_map = (HashMap*)main_entry->value;
    int deleted_count = 0;
    for (size_t i = 0; i < num_fields; ++i) {
        if (store_delete_entry_from_map(inner_map, fields[i])) {
            deleted_count++;
        }
    }
    return deleted_count;
}

int store_hgetall(HashMap* map, const char* key, char*** out_results, size_t* out_count) {
    if (!map || !key || !out_results || !out_count) return -1; // Invalid input

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (strcmp(current->key, key) == 0) {
            main_entry = current;
            break;
        }
        current = current->next;
    }

    if (!main_entry) {
        // Key not found, consistent with Redis HGETALL returning an empty array
        *out_results = NULL;
        *out_count = 0;
        return 0;
    }

    if (main_entry->type != OBJ_HASH) {
        // Key found but is not a hash
        *out_results = NULL;
        *out_count = 0;
        return -1; // Indicate wrong type
    }

    HashMap* inner_map = (HashMap*)main_entry->value;
    int num_fields = count_hash_entries(inner_map);

    if (num_fields == 0) {
        *out_results = NULL;
        *out_count = 0;
        return 0; // Empty hash, but valid
    }

    // Allocate array for results (field1, value1, field2, value2, ...)
    *out_count = (size_t)num_fields * 2;
    *out_results = (char**)malloc(sizeof(char*) * (*out_count));
    if (!*out_results) {
        *out_count = 0;
        return -1; // OOM
    }

    size_t result_idx = 0;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        Entry* inner_current = inner_map->buckets[i];
        while (inner_current) {
            if (result_idx < *out_count) {
                (*out_results)[result_idx++] = strdup(inner_current->key);
                if (!(*out_results)[result_idx - 1]) {
                    // OOM during strdup, clean up and return error
                    for (size_t j = 0; j < result_idx - 1; ++j) {
                        free((*out_results)[j]);
                    }
                    free(*out_results);
                    *out_results = NULL;
                    *out_count = 0;
                    return -1;
                }
            } else {
                // Should not happen if num_fields was counted correctly
                // but as a safeguard, break and indicate error
                for (size_t j = 0; j < result_idx; ++j) {
                    free((*out_results)[j]);
                }
                free(*out_results);
                *out_results = NULL;
                *out_count = 0;
                return -1;
            }

            if (result_idx < *out_count) {
                // Note: inner_current->value is a char* because the inner map stores strings
                (*out_results)[result_idx++] = strdup((char*)inner_current->value);
                if (!(*out_results)[result_idx - 1]) {
                    // OOM during strdup, clean up and return error
                    for (size_t j = 0; j < result_idx - 1; ++j) {
                        free((*out_results)[j]);
                    }
                    free(*out_results);
                    *out_results = NULL;
                    *out_count = 0;
                    return -1;
                }
            } else {
                // Safeguard
                for (size_t j = 0; j < result_idx; ++j) {
                    free((*out_results)[j]);
                }
                free(*out_results);
                *out_results = NULL;
                *out_count = 0;
                return -1;
            }
            inner_current = inner_current->next;
        }
    }
    return 0; // Success
}

int store_del(HashMap* map, const char** keys, size_t num_keys) {
    if (!map || !keys || num_keys == 0) return 0;

    int deleted_count = 0;
    for (size_t i = 0; i < num_keys; ++i) {
        if (store_delete_entry_from_map(map, keys[i])) {
            deleted_count++;
        }
    }
    return deleted_count;
}

int store_exists(HashMap* map, const char* key) {
    if (!map || !key) return 0;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    while (current) {
        if (strcmp(current->key, key) == 0) {
            return 1; // Key found
        }
        current = current->next;
    }
    return 0; // Key not found
}

ObjType store_type(HashMap* map, const char* key) {
    if (!map || !key) return (ObjType)-1; // Indicate not found/error

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    while (current) {
        if (strcmp(current->key, key) == 0) {
            return current->type;
        }
        current = current->next;
    }
    return (ObjType)-1; // Key not found
}

void store_free(HashMap* map) {
    if (!map) return;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        Entry* current = map->buckets[i];
        while (current) {
            Entry* to_free = current;
            current = current->next;
            free(to_free->key);
            // Free value based on its type
            if (to_free->type == OBJ_STRING) {
                free((char*)to_free->value);
            } else if (to_free->type == OBJ_HASH) {
                // Recursively free the inner hash map
                store_free((HashMap*)to_free->value);
                free((HashMap*)to_free->value); // Free the HashMap struct itself
            }
            free(to_free);
        }
        map->buckets[i] = NULL;
    }
}

void store_flushdb(HashMap* map) {
    if (!map) return;
    store_free(map); // Free all existing entries
    store_init(map); // Re-initialize the hash map
}

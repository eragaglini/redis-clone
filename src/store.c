#include "store.h"
#include "protocol.h"
#include <stdlib.h> // For malloc, free
#include <string.h> // For strdup, strcmp
#include <stdio.h>  // For snprintf in debug
#include <stdbool.h>

// --- Private Helper Functions ---

static uint32_t jenkins_hash(const char* key, size_t len) {
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash += (uint8_t)key[i]; // Cast a uint8_t per gestire correttamente i byte
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

static uint32_t get_bucket_index(const char* key, size_t len) {
    return jenkins_hash(key, len) % HASH_MAP_SIZE;
}
int arg_equals(const struct Argument* arg1, const struct Argument* arg2) {
    return (
        arg1->len == arg2->len &&
        memcmp(arg1->data, arg2->data, arg1->len) == 0
        );
}

// --- Public Functions Implementation ---

void store_init(HashMap* map) {
    if (!map) return;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        map->buckets[i] = NULL;
    }
}

int store_set(HashMap* map, const struct Argument* key, const struct Argument* value) {
    if (!map || !key || !value) return -1;

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    // 1. Check if key already exists
    while (current) {
        if (arg_equals(current->key, key)) {
            if (current->type != OBJ_STRING) return -1; // Type mismatch

            // Aggiornamento: Deep Copy nel campo union 'str'
            char* new_data = (char*)malloc(value->len);
            if (!new_data) return -1;

            memcpy(new_data, value->data, value->len);
            if (current->val.str->data) free(current->val.str->data);

            current->val.str->data = new_data;
            current->val.str->len = value->len;
            return 0;
        }
        current = current->next;
    }

    // 2. Key does not exist, create new entry
    Entry* new_entry = (Entry*)malloc(sizeof(Entry));
    if (!new_entry) return -1;

    // Deep copy della CHIAVE
    new_entry->key = (struct Argument*)malloc(sizeof(struct Argument));
    if (!new_entry->key) { free(new_entry); return -1; }
    new_entry->key->data = (char*)malloc(key->len);
    if (!new_entry->key->data) { free(new_entry->key); free(new_entry); return -1; }
    memcpy(new_entry->key->data, key->data, key->len);
    new_entry->key->len = key->len;

    // Deep copy del VALORE (nella union str)
    new_entry->val.str = (struct Argument*)malloc(sizeof(struct Argument));
    if (!new_entry->val.str) { free(new_entry->key->data); free(new_entry->key); free(new_entry); return -1; }
    new_entry->val.str->data = (char*)malloc(value->len);
    if (!new_entry->val.str->data) {
        free(new_entry->val.str);
        free(new_entry->key->data);
        free(new_entry->key);
        free(new_entry);
        return -1;
    }
    memcpy(new_entry->val.str->data, value->data, value->len);
    new_entry->val.str->len = value->len;

    new_entry->type = OBJ_STRING;
    new_entry->next = map->buckets[index];
    map->buckets[index] = new_entry;

    return 1;
}

struct Argument* store_get(HashMap* map, const struct Argument* key) {
    if (!map || !key) return NULL;

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    while (current) {
        if (arg_equals(current->key, key)) {
            if (current->type == OBJ_STRING) {
                // Restituiamo il riferimento alla struct Argument dentro la union
                return current->val.str;
            }
            return NULL;
        }
        current = current->next;
    }
    return NULL;
}

int store_delete_entry_from_map(HashMap* map, const struct Argument* key) {
    if (!map || !key) return 0;

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];
    Entry* prev = NULL;

    while (current) {
        if (arg_equals(current->key, key)) {
            // 1. Sganciamo l'entry dalla lista (Puntatori)
            if (prev) {
                prev->next = current->next;
            } else {
                map->buckets[index] = current->next;
            }

            // 2. Libera la CHIAVE
            if (current->key->data) {
                free(current->key->data);
            }
            free(current->key);

            // 3. Libera il VALORE in base al tipo
            if (current->type == OBJ_STRING) {
                if (current->val.str->data) {
                    free(current->val.str->data);
                }
                free(current->val.str);
            } 
            else if (current->type == OBJ_HASH) {
                if (current->val.hmap) {
                    // LIBERA I BUCKET INTERNI
                    store_free(current->val.hmap); 
                    // LIBERA LA STRUCT HASHMAP STESSA
                    free(current->val.hmap); 
                }
            }

            // 4. Libera la struct Entry
            free(current);
            return 1; 
        }
        prev = current;
        current = current->next;
    }
    return 0;
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

int store_hset(HashMap* map, const struct Argument* key, const struct Argument* field, const struct Argument* value) {
    if (!map || !key || !field || !value) return -1; // -1 for error

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (arg_equals(current->key, key)) {
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

        // Deep copy of the key
        new_main_entry->key = (struct Argument*)malloc(sizeof(struct Argument));
        if (!new_main_entry->key) { free(new_main_entry); free(new_hash_map); return -1; }
        new_main_entry->key->data = (char*)malloc(key->len);
        if (!new_main_entry->key->data) { free(new_main_entry->key); free(new_main_entry); free(new_hash_map); return -1; }
        memcpy(new_main_entry->key->data, key->data, key->len);
        new_main_entry->key->len = key->len;

        new_main_entry->val.hmap = new_hash_map; // Value is the inner HashMap*
        new_main_entry->type = OBJ_HASH;
        new_main_entry->next = NULL;

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

    }
    else {
        // Main key exists
        if (main_entry->type != OBJ_HASH) {
            // Key exists but is not a hash, cannot perform HSET
            return -1; // Type mismatch error
        }
        // Key is a hash, perform HSET on the inner hash map
        return store_set(main_entry->val.hmap, field, value);
    }
}

struct Argument* store_hget(HashMap* map, const struct Argument* key, const struct Argument* field) {
    if (!map || !key || !field) return NULL;

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    // Find the main key entry
    while (current) {
        if (arg_equals(current->key, key)) {
            if (current->type == OBJ_HASH) {
                // Key is a hash, perform HGET on the inner hash map
                return store_get(current->val.hmap, field);
            }
            else {
                // Key exists but is not a hash
                return NULL;
            }
        }
        current = current->next;
    }
    return NULL; // Key not found
}

int store_hlen(HashMap* map, const struct Argument* key) {
    if (!map || !key) return 0; // Return 0 for invalid input, consistent with non-existent key

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    // Find the main key entry
    while (current) {
        if (arg_equals(current->key, key)) {
            if (current->type == OBJ_HASH) {
                // Key is a hash, return the count of entries in the inner hash map
                return count_hash_entries(current->val.hmap);
            }
            else {
                // Key exists but is not a hash, return 0 (as per Redis HLEN behavior)
                return 0;
            }
        }
        current = current->next;
    }
    return 0; // Key not found, return 0 as per Redis HLEN behavior
}

int store_hdel(HashMap* map, const struct Argument* key, const struct Argument* fields, size_t num_fields) {
    if (!map || !key || !fields || num_fields == 0) return 0; // Invalid input, return 0 as per Redis HDEL if no fields or invalid map/key

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (arg_equals(current->key, key)) {
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

    HashMap* inner_map = main_entry->val.hmap;
    int deleted_count = 0;
    for (size_t i = 0; i < num_fields; ++i) {
        if (store_delete_entry_from_map(inner_map, &fields[i])) {
            deleted_count++;
        }
    }
    return deleted_count;
}

int store_hgetall(HashMap* map, const struct Argument* key, struct Argument** out_results, size_t* out_count) {
    if (!map || !key || !out_results || !out_count) return -1; // Invalid input

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];
    Entry* main_entry = NULL;

    // Find the main key entry
    while (current) {
        if (arg_equals(current->key, key)) {
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

    HashMap* inner_map = main_entry->val.hmap;
    int num_fields = count_hash_entries(inner_map);

    if (num_fields == 0) {
        *out_results = NULL;
        *out_count = 0;
        return 0; // Empty hash, but valid
    }

    // Allocate array for results (field1, value1, field2, value2, ...)
    *out_count = (size_t)num_fields * 2;
    *out_results = (struct Argument*)malloc(sizeof(struct Argument) * (*out_count));
    if (!*out_results) {
        *out_count = 0;
        return -1; // OOM
    }

    size_t result_idx = 0;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        Entry* inner_current = inner_map->buckets[i];
        while (inner_current) {
            if (result_idx < *out_count) {
                (*out_results)[result_idx].data = (char*)malloc(inner_current->key->len);
                if (!(*out_results)[result_idx].data) {
                    // OOM, clean up and return error
                    for (size_t j = 0; j < result_idx; ++j) {
                        free((*out_results)[j].data);
                    }
                    free(*out_results);
                    *out_results = NULL;
                    *out_count = 0;
                    return -1;
                }
                memcpy((*out_results)[result_idx].data, inner_current->key->data, inner_current->key->len);
                (*out_results)[result_idx].len = inner_current->key->len;
                result_idx++;
            }
            else {
                // Should not happen if num_fields was counted correctly
                // but as a safeguard, break and indicate error
                for (size_t j = 0; j < result_idx; ++j) {
                    free((*out_results)[j].data);
                }
                free(*out_results);
                *out_results = NULL;
                *out_count = 0;
                return -1;
            }

            if (result_idx < *out_count) {
                // Note: inner_current->value is a char* because the inner map stores strings
                (*out_results)[result_idx].data = (char*)malloc(inner_current->val.str->len);
                if (!(*out_results)[result_idx].data) {
                    // OOM, clean up and return error
                    for (size_t j = 0; j < result_idx; ++j) {
                        free((*out_results)[j].data);
                    }
                    free(*out_results);
                    *out_results = NULL;
                    *out_count = 0;
                    return -1;
                }
                memcpy((*out_results)[result_idx].data, inner_current->val.str->data, inner_current->val.str->len);
                (*out_results)[result_idx].len = inner_current->val.str->len;
                result_idx++;
            }
            else {
                // Safeguard
                for (size_t j = 0; j < result_idx; ++j) {
                    free((*out_results)[j].data);
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

int store_del(HashMap* map, const struct Argument* keys, size_t num_keys) {
    if (!map || !keys || num_keys == 0) return 0;

    int deleted_count = 0;
    for (size_t i = 0; i < num_keys; ++i) {
        if (store_delete_entry_from_map(map, &keys[i])) {
            deleted_count++;
        }
    }
    return deleted_count;
}

int store_exists(HashMap* map, const struct Argument* key) {
    if (!map || !key) return 0;

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    while (current) {
        if (arg_equals(current->key, key)) {
            return 1; // Key found
        }
        current = current->next;
    }
    return 0; // Key not found
}

ObjType store_type(HashMap* map, const struct Argument* key) {
    if (!map || !key) return (ObjType)-1; // Indicate not found/error

    uint32_t index = get_bucket_index(key->data, key->len);
    Entry* current = map->buckets[index];

    while (current) {
        if (arg_equals(current->key, key)) {
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

            // 1. Libera i dati della chiave (Deep Copy fatta in store_set)
            if (to_free->key->data) {
                free(to_free->key->data);
            }
            free(to_free->key);

            // 2. Libera il valore in base al tipo (OBJ_STRING o OBJ_HASH)
            if (to_free->type == OBJ_STRING) {
                if (to_free->val.str->data) {
                    free(to_free->val.str->data);
                }
                free(to_free->val.str);
            }
            else if (to_free->type == OBJ_HASH) {
                // RICORSIONE: Libera la HashMap interna
                if (to_free->val.hmap) {
                    store_free(to_free->val.hmap);
                    // Dopo che store_free ha pulito i bucket interni, 
                    // liberiamo la struct HashMap stessa.
                    free(to_free->val.hmap);
                }
            }

            // 3. Libera la struct Entry stessa
            free(to_free);
        }
        map->buckets[i] = NULL;
    }
    // Nota: la liberazione della struct 'map' stessa di solito 
    // viene fatta dal chiamante o alla fine di questa funzione 
    // se non è la mappa globale.
}

void store_flushdb(HashMap* map) {
    if (!map) return;
    store_free(map); // Free all existing entries
    store_init(map); // Re-initialize the hash map
}

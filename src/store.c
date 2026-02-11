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
    if (!map || !key || !value) return 0;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];
    Entry* prev = NULL;

    // Check if key already exists, update value
    while (current) {
        if (strcmp(current->key, key) == 0) {
            free(current->value); // Free old value
            current->value = strdup(value);
            if (!current->value) return 0; // OOM
            return 1;
        }
        prev = current;
        current = current->next;
    }

    // Key does not exist, create new entry
    Entry* new_entry = (Entry*)malloc(sizeof(Entry));
    if (!new_entry) return 0; // OOM

    new_entry->key = strdup(key);
    new_entry->value = strdup(value);
    new_entry->next = NULL;

    if (!new_entry->key || !new_entry->value) {
        free(new_entry->key);
        free(new_entry->value);
        free(new_entry);
        return 0; // OOM
    }

    // Add to the beginning of the bucket's linked list
    new_entry->next = map->buckets[index];
    map->buckets[index] = new_entry;

    return 1;
}

char* store_get(HashMap* map, const char* key) {
    if (!map || !key) return NULL;

    uint32_t index = get_bucket_index(key);
    Entry* current = map->buckets[index];

    while (current) {
        if (strcmp(current->key, key) == 0) {
            return strdup(current->value); // Return a copy, caller frees
        }
        current = current->next;
    }
    return NULL; // Key not found
}

int store_del(HashMap* map, const char* key) {
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
            free(current->value);
            free(current);
            return 1; // Deleted
        }
        prev = current;
        current = current->next;
    }
    return 0; // Key not found
}

void store_free(HashMap* map) {
    if (!map) return;
    for (int i = 0; i < HASH_MAP_SIZE; ++i) {
        Entry* current = map->buckets[i];
        while (current) {
            Entry* to_free = current;
            current = current->next;
            free(to_free->key);
            free(to_free->value);
            free(to_free);
        }
        map->buckets[i] = NULL;
    }
}

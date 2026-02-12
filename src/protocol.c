#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <strings.h> // Required for strcasecmp
#include "store.h"   // Required for HashMap

#ifdef DEBUG
#define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#endif

void free_argv(Conn* c) {
    if (c->argv) {
        DEBUG_PRINTF("DEBUG: Freeing argv for connection %p\n", (void*)c);
        for (uint32_t i = 0; i < c->argc; ++i) {
            if (c->argv[i]) {
                free(c->argv[i]);
            }
        }
        free(c->argv);
        c->argv = NULL;
    }
}

static void conn_error(Conn* c, const char* msg);

// Crea un nuovo comando e lo appende alla lista specificata (head, tail).
// Restituisce il puntatore al nuovo comando in caso di successo, NULL altrimenti.
Command* cmd_create_and_append(Conn* c, char** argv, uint32_t argc, Command** head, Command** tail) {
    DEBUG_PRINTF("DEBUG: Creating and appending command for connection %p: argc = %u\n", (void*)c, argc);
    Command* cmd = (Command*)malloc(sizeof(Command));
    if (!cmd) {
        conn_error(c, "OOM during command allocation");
        return NULL;
    }
    memset(cmd, 0, sizeof(Command));

    cmd->argc = argc;
    cmd->argv = argv; // Assume ownership of argv
    cmd->next = NULL;

    if (*head == NULL) {
        *head = cmd;
        *tail = cmd;
    }
    else {
        (*tail)->next = cmd;
        *tail = cmd;
    }
    DEBUG_PRINTF("DEBUG: Command appended. Head: %p, Tail: %p\n", (void*)*head, (void*)*tail);
    return cmd;
}

// Libera tutti i comandi nella lista della connessione.
void free_command_list(Conn* c) {
    DEBUG_PRINTF("DEBUG: Freeing command list for connection %p\n", (void*)c);
    Command* cmd = c->cmd_list_head;
    while (cmd) {
        Command* next = cmd->next;
        if (cmd->argv) {
            for (uint32_t i = 0; i < cmd->argc; ++i) {
                if (cmd->argv[i]) {
                    free(cmd->argv[i]);
                }
            }
            free(cmd->argv);
        }
        free(cmd);
        cmd = next;
    }
    c->cmd_list_head = NULL;
    c->cmd_list_tail = NULL;
    DEBUG_PRINTF("DEBUG: Command list freed for connection %p\n", (void*)c);
}

// Libera tutti i comandi nella lista di comandi accodati per la transazione.
void free_queued_command_list(Conn* c) {
    DEBUG_PRINTF("DEBUG: Freeing queued command list for connection %p\n", (void*)c);
    Command* cmd = c->queued_cmds_head;
    while (cmd) {
        Command* next = cmd->next;
        if (cmd->argv) {
            for (uint32_t i = 0; i < cmd->argc; ++i) {
                if (cmd->argv[i]) {
                    free(cmd->argv[i]);
                }
            }
            free(cmd->argv);
        }
        free(cmd);
        cmd = next;
    }
    c->queued_cmds_head = NULL;
    c->queued_cmds_tail = NULL;
    DEBUG_PRINTF("DEBUG: Queued command list freed for connection %p\n", (void*)c);
}

static void conn_error(Conn* c, const char* msg) {
    c->error = 1;
    DEBUG_PRINTF("DEBUG: Connection error for %p: %s\n", (void*)c, msg);
}

static void consume_bytes_from_buffer(Conn* c, size_t bytes) {
    DEBUG_PRINTF("DEBUG: Consuming %zu bytes from buffer for connection %p\n", bytes, (void*)c);
    size_t remaining = c->rbuf_size - bytes;
    if (remaining > 0) {
        memmove(c->rbuf, &c->rbuf[bytes], remaining);
    }
    c->rbuf_size = remaining;
    DEBUG_PRINTF("DEBUG: Consumed %zu bytes from buffer for connection %p, rbuf_size: %zu\n", bytes, (void*)c, c->rbuf_size);
}

// Esegue un singolo comando e restituisce la sua risposta in una stringa allocata dinamicamente.
// La stringa restituita deve essere liberata dal chiamante.
char* get_command_reply(Conn* c, Command* cmd, HashMap* store) {
    // Usiamo una dimensione maggiore per il buffer temporaneo per le risposte,
    // specialmente per gestire risposte a array o stringhe bulk più lunghe.
    // Un K_MAX_MSG * 2 dovrebbe essere sufficiente, considerando che K_MAX_MSG è 4096.
    char reply_buf[K_MAX_MSG * 2];
    int len = 0;



    if (cmd->argc == 0 || cmd->argv[0] == NULL) {
        const char* reply_str = "OK";
        len = snprintf(reply_buf, sizeof(reply_buf), "+%s\r\n", reply_str);
    }
    else if (strcasecmp(cmd->argv[0], "PING") == 0) {
        if (cmd->argc > 1 && cmd->argv[1] != NULL) {
            char* message = cmd->argv[1];
            int msg_len = strlen(message);
            len = snprintf(reply_buf, sizeof(reply_buf), "$%d\r\n%s\r\n", msg_len, message);
        }
        else {
            const char* reply_str = "PONG";
            len = snprintf(reply_buf, sizeof(reply_buf), "+%s\r\n", reply_str);
        }
    }
    else if (strcasecmp(cmd->argv[0], "SET") == 0) {
        if (cmd->argc != 3) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'set' command\r\n");
        }
        else {
            int set_result = store_set(store, cmd->argv[1], cmd->argv[2]);
            if (set_result == 1 || set_result == 0) { // 1 for new key, 0 for updated key
                const char* reply_str = "OK";
                len = snprintf(reply_buf, sizeof(reply_buf), "+%s\r\n", reply_str);
            } else { // -1 for OOM or type mismatch error
                len = snprintf(reply_buf, sizeof(reply_buf), "-ERR OOM during SET\r\n");
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "GET") == 0) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'get' command\r\n");
        }
        else {
            char* value = store_get(store, cmd->argv[1]);
            if (value) {
                int val_len = strlen(value);
                len = snprintf(reply_buf, sizeof(reply_buf), "$%d\r\n%s\r\n", val_len, value);
                free(value); // Free the duplicated string from store_get
            }
            else {
                const char* reply_str = "-1";
                len = snprintf(reply_buf, sizeof(reply_buf), "$%s\r\n", reply_str);
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "HSET") == 0) {
        if (cmd->argc < 4 || (cmd->argc - 2) % 2 != 0) { // HSET key field value [field value ...] must have at least 4 args and an even number of field-value pairs
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'hset' command\r\n");
        }
        else {
            int new_fields_count = 0;
            // Iterate through field-value pairs. Start from cmd->argv[2] as cmd->argv[0] is HSET, cmd->argv[1] is key.
            for (uint32_t i = 2; i < cmd->argc; i += 2) {
                const char* field = cmd->argv[i];
                const char* value = cmd->argv[i+1];
                int hset_result = store_hset(store, cmd->argv[1], field, value);
                if (hset_result == 1) { // New field added
                    new_fields_count++;
                } else if (hset_result == -1) { // Error during HSET (e.g., wrong type on key)
                    len = snprintf(reply_buf, sizeof(reply_buf), "-ERR HSET failed: %s is not a hash or other error\r\n", cmd->argv[1]);
                    // Break loop and return error immediately
                    new_fields_count = -1; // Indicate error
                    break;
                }
            }
            if (new_fields_count != -1) {
                len = snprintf(reply_buf, sizeof(reply_buf), ":%d\r\n", new_fields_count);
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "HGET") == 0) {
        if (cmd->argc != 3) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'hget' command\r\n");
        }
        else {
            char* value = store_hget(store, cmd->argv[1], cmd->argv[2]);
            if (value) {
                int val_len = strlen(value);
                len = snprintf(reply_buf, sizeof(reply_buf), "$%d\r\n%s\r\n", val_len, value);
                free(value); // Free the duplicated string from store_hget
            }
            else {
                const char* reply_str = "-1";
                len = snprintf(reply_buf, sizeof(reply_buf), "$%s\r\n", reply_str); // Nil bulk string
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "HLEN") == 0) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'hlen' command\r\n");
        }
        else {
            int hlen_result = store_hlen(store, cmd->argv[1]);
            // hlen_result can be >= 0 (count) or -1 (error/not hash)
            // Redis HLEN returns 0 if key does not exist or is not a hash
            len = snprintf(reply_buf, sizeof(reply_buf), ":%d\r\n", hlen_result);
        }
    }
    else if (strcasecmp(cmd->argv[0], "HDEL") == 0) {
        if (cmd->argc < 3) { // HDEL key field [field ...] must have at least 3 args
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'hdel' command\r\n");
        }
        else {
            // cmd->argv[1] is the key, cmd->argv[2] onwards are fields
            // The fields array starts from cmd->argv[2]
            // num_fields is cmd->argc - 2
            int hdel_result = store_hdel(store, cmd->argv[1], (const char**)&cmd->argv[2], cmd->argc - 2);
            if (hdel_result >= 0) { // Number of deleted fields (can be 0)
                len = snprintf(reply_buf, sizeof(reply_buf), ":%d\r\n", hdel_result);
            } else if (hdel_result == -1) { // Wrong type
                len = snprintf(reply_buf, sizeof(reply_buf), "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
            } else { // Other potential errors from store_hdel
                len = snprintf(reply_buf, sizeof(reply_buf), "-ERR HDEL failed\r\n");
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "HGETALL") == 0) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'hgetall' command\r\n");
        }
        else {
            char** results = NULL;
            size_t count = 0;
            int hgetall_status = store_hgetall(store, cmd->argv[1], &results, &count);

            if (hgetall_status == 0) { // Success
                // Format as RESP array: *<num_elements>\r\n$<len1>\r\n<val1>\r\n...
                size_t total_resp_len = 0;
                // Calculate length needed for all elements
                for (size_t i = 0; i < count; ++i) {
                    total_resp_len += snprintf(NULL, 0, "$%zu\r\n%s\r\n", strlen(results[i]), results[i]);
                }
                // Add length for array header (*<count>\r\n) and null terminator
                total_resp_len += snprintf(NULL, 0, "*%zu\r\n", count);

                char* resp_str = (char*)malloc(total_resp_len + 1); // +1 for null terminator
                if (!resp_str) {
                    len = snprintf(reply_buf, sizeof(reply_buf), "-ERR OOM during HGETALL reply construction\r\n");
                } else {
                    int offset = snprintf(resp_str, total_resp_len + 1, "*%zu\r\n", count);
                    for (size_t i = 0; i < count; ++i) {
                        offset += snprintf(resp_str + offset, total_resp_len + 1 - offset, "$%zu\r\n%s\r\n", strlen(results[i]), results[i]);
                        free(results[i]); // Free individual string
                    }
                    free(results); // Free array of pointers
                    strncpy(reply_buf, resp_str, sizeof(reply_buf) - 1);
                    reply_buf[sizeof(reply_buf) - 1] = '\0'; // Ensure null termination
                    len = strlen(reply_buf);
                    free(resp_str); // Free the dynamically allocated RESP string
                }
            } else if (hgetall_status == -1) { // Key not found or not a hash (wrong type)
                len = snprintf(reply_buf, sizeof(reply_buf), "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "DEL") == 0) {
        if (cmd->argc < 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'del' command\r\n");
        }
        else {
            // cmd->argv[1] is the first key, cmd->argc - 1 is the number of keys
            int deleted_count = store_del(store, (const char**)&cmd->argv[1], cmd->argc - 1);
            len = snprintf(reply_buf, sizeof(reply_buf), ":%d\r\n", deleted_count);
        }
    }
    else if (strcasecmp(cmd->argv[0], "EXISTS") == 0) {
        if (cmd->argc != 2) { // Implementing for single key EXISTS for now
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'exists' command\r\n");
        }
        else {
            int exists_result = store_exists(store, cmd->argv[1]);
            len = snprintf(reply_buf, sizeof(reply_buf), ":%d\r\n", exists_result);
        }
    }
    else if (strcasecmp(cmd->argv[0], "TYPE") == 0) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'type' command\r\n");
        }
        else {
            ObjType type = store_type(store, cmd->argv[1]);
            if (type == OBJ_STRING) {
                len = snprintf(reply_buf, sizeof(reply_buf), "+string\r\n");
            } else if (type == OBJ_HASH) {
                len = snprintf(reply_buf, sizeof(reply_buf), "+hash\r\n");
            } else { // (ObjType)-1 for not found
                len = snprintf(reply_buf, sizeof(reply_buf), "+none\r\n");
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "TYPE") == 0) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'type' command\r\n");
        }
        else {
            ObjType type = store_type(store, cmd->argv[1]);
            if (type == OBJ_STRING) {
                len = snprintf(reply_buf, sizeof(reply_buf), "+string\r\n");
            } else if (type == OBJ_HASH) {
                len = snprintf(reply_buf, sizeof(reply_buf), "+hash\r\n");
            } else { // (ObjType)-1 for not found
                len = snprintf(reply_buf, sizeof(reply_buf), "+none\r\n");
            }
        }
    }
    else if (strcasecmp(cmd->argv[0], "FLUSHDB") == 0) {
        if (cmd->argc != 1) {
            len = snprintf(reply_buf, sizeof(reply_buf), "-ERR wrong number of arguments for 'flushdb' command\r\n");
        } else {
            store_flushdb(store);
            len = snprintf(reply_buf, sizeof(reply_buf), "+OK\r\n");
        }
    }
    else {
        // Unknown command
        const char* reply_str = "OK"; // Default to OK for unknown commands
        len = snprintf(reply_buf, sizeof(reply_buf), "+%s\r\n", reply_str);
    }

    if (len <= 0 || len >= sizeof(reply_buf)) {
        // Handle snprintf error or buffer overflow
        DEBUG_PRINTF("ERROR: snprintf failed or buffer overflow in get_command_reply.\n");
        return strdup("-ERR Internal server error\r\n");
    }

    // Return a dynamically allocated copy of the reply
    return strdup(reply_buf);
}

// Esegue i comandi parsati e prepara le risposte.
void execute_command(Conn* c, HashMap* store) {
    DEBUG_PRINTF("DEBUG: execute_command called for connection %p\n", (void*)c);
    while (c->cmd_list_head != NULL) {
        Command* cmd = c->cmd_list_head;
        c->cmd_list_head = cmd->next;
        if (c->cmd_list_head == NULL) {
            c->cmd_list_tail = NULL; // List is now empty
        }



        char* reply = NULL; // Dynamically allocated reply string

        // Handle transaction commands
        if (cmd->argc > 0 && cmd->argv[0] != NULL) {
            if (strcasecmp(cmd->argv[0], "MULTI") == 0) {
                if (c->in_transaction) {
                    reply = strdup("-ERR MULTI already in progress\r\n");
                } else {
                    c->in_transaction = true;
                    reply = strdup("+OK\r\n");
                }
            } else if (strcasecmp(cmd->argv[0], "EXEC") == 0) {
                if (!c->in_transaction) {
                    reply = strdup("-ERR EXEC without MULTI\r\n");
                } else {
                    // Execute all queued commands
                    c->in_transaction = false; // Transaction ends with EXEC
                    if (c->queued_cmds_head == NULL) {
                        reply = strdup("*0\r\n"); // Empty array reply if no commands were queued
                    } else {
                        // Build array of results for EXEC
                        // First, calculate total length needed for all replies
                        size_t total_replies_len = 0;
                        Command* current_queued_cmd = c->queued_cmds_head;
                        // Temporary storage for individual replies from queued commands
                        char* individual_replies[K_MAX_MSG]; // Max K_MAX_MSG queued commands
                        int reply_count = 0;

                        while (current_queued_cmd != NULL) {
                            if (reply_count >= K_MAX_MSG) {
                                DEBUG_PRINTF("WARNING: Too many queued commands, truncating EXEC response.\n");
                                break;
                            }
                            // Execute the queued command and get its reply
                            char* q_reply = get_command_reply(c, current_queued_cmd, store);
                            if (q_reply) {
                                individual_replies[reply_count++] = q_reply;
                                total_replies_len += strlen(q_reply);
                            } else {
                                individual_replies[reply_count++] = strdup("-ERR Internal server error during EXEC\r\n");
                                total_replies_len += strlen(individual_replies[reply_count - 1]);
                            }
                            current_queued_cmd = current_queued_cmd->next;
                        }

                        // Allocate buffer for the final EXEC array reply
                        // Format: *<num_replies>\r\n<reply1_str><reply2_str>...
                        // Add room for *<num_replies>\r\n and null terminator
                        char* exec_reply_buf = (char*)malloc(total_replies_len + 32); // 32 is a generous estimate for *<num>\r\n
                        if (!exec_reply_buf) {
                            reply = strdup("-ERR OOM during EXEC reply construction\r\n");
                            // Free individual replies if OOM
                            for (int i = 0; i < reply_count; ++i) {
                                free(individual_replies[i]);
                            }
                        } else {
                            int offset = snprintf(exec_reply_buf, total_replies_len + 32, "*%d\r\n", reply_count);
                            for (int i = 0; i < reply_count; ++i) {
                                offset += snprintf(exec_reply_buf + offset, total_replies_len + 32 - offset, "%s", individual_replies[i]);
                                free(individual_replies[i]); // Free individual reply
                            }
                            reply = exec_reply_buf;
                        }
                    }
                    free_queued_command_list(c); // Clear the queued commands after EXEC
                }
            } else if (strcasecmp(cmd->argv[0], "DISCARD") == 0) {
                if (!c->in_transaction) {
                    reply = strdup("-ERR DISCARD without MULTI\r\n");
                } else {
                    c->in_transaction = false;
                    free_queued_command_list(c); // Discard queued commands
                    reply = strdup("+OK\r\n");
                }
            } else if (c->in_transaction) {
                // If in transaction, queue the command
                // cmd_create_and_append will take ownership of cmd->argv
                cmd_create_and_append(c, cmd->argv, cmd->argc, &c->queued_cmds_head, &c->queued_cmds_tail);
                // Important: clear cmd->argv from the original cmd struct
                // so it's not freed twice later.
                cmd->argv = NULL; // Ownership transferred to queued command.
                reply = strdup("+QUEUED\r\n");
            }
        }

        // If not a transaction command or not in transaction, execute normally
        if (reply == NULL) {
            reply = get_command_reply(c, cmd, store);
        }

        if (reply) {
            // Append reply to write buffer
            size_t reply_len = strlen(reply);
            if (c->wbuf_size + reply_len < sizeof(c->wbuf)) {
                memcpy(c->wbuf + c->wbuf_size, reply, reply_len);
                c->wbuf_size += reply_len;
            } else {
                DEBUG_PRINTF("ERROR: Write buffer overflow, dropping reply.\n");
                // In a real server, would need to handle this by flushing wbuf or resizing.
            }
            free(reply); // Free the dynamically allocated reply
        }

        // Free the command and its arguments (if not transferred to queued list)
        if (cmd->argv) { // Check if ownership was transferred to queued_cmds.
            for (uint32_t i = 0; i < cmd->argc; ++i) {
                if (cmd->argv[i]) {
                    free(cmd->argv[i]);
                }
            }
            free(cmd->argv);
        }
        free(cmd);
    }
}

void consume_buffer(Conn* c) {
    DEBUG_PRINTF("DEBUG: consume_buffer called for connection %p, rbuf_size: %zu, parse_state: %d\n", (void*)c, c->rbuf_size, c->parse_state);

    if (c->error) {
        DEBUG_PRINTF("DEBUG: Connection %p in error state.\n", (void*)c);
        // If there's an error, clear any partially parsed command's argv to avoid leaks.
        free_argv(c);
        return;
    }

    while (1) {
        DEBUG_PRINTF("DEBUG: Connection %p, current parse_state: %d, rbuf_size: %zu\n", (void*)c, c->parse_state, c->rbuf_size);
        switch (c->parse_state) {
        case RESP_PARSE_TYPE:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_TYPE\n", (void*)c);
            // Inizializza argv a NULL per prevenire doppi free se il parsing fallisce prima di allocare.
            // c->argv = NULL;
            // c->argc = 0; // Inizializza anche argc

            if (c->rbuf_size == 0) {
                DEBUG_PRINTF("DEBUG: Connection %p, rbuf_size is 0, returning false.\n", (void*)c);
                return;
            }
            c->resp_type = c->rbuf[0];
            if (c->resp_type != '$' && c->resp_type != '*') {
                DEBUG_PRINTF("DEBUG: Connection %p, Invalid RESP type: %c\n", (void*)c, c->resp_type);
                conn_error(c, "Invalid RESP type");
                return;
            }
            DEBUG_PRINTF("DEBUG: Connection %p, Parsed RESP Type: %c\n", (void*)c, c->resp_type);
            consume_bytes_from_buffer(c, 1);
            c->parse_state = RESP_PARSE_LEN;
            // Fallthrough intenzionale

        case RESP_PARSE_LEN:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_LEN\n", (void*)c);
            { // blocco per variabili locali
                char* crlf = (char*)memchr(c->rbuf, '\r', c->rbuf_size);
                if (!crlf || crlf + 1 >= (char*)c->rbuf + c->rbuf_size || *(crlf + 1) != '\n') {
                    DEBUG_PRINTF("DEBUG: Connection %p, CRLF not found or incomplete, returning false.\n", (void*)c);
                    return;
                }

                int len_str_len = crlf - (char*)c->rbuf;
                char len_str[32];
                if (len_str_len >= sizeof(len_str)) {
                    conn_error(c, "Length string too long");
                    return;
                }
                memcpy(len_str, c->rbuf, len_str_len);
                len_str[len_str_len] = '\0';

                long long parsed_len = strtoll(len_str, NULL, 10);
                DEBUG_PRINTF("DEBUG: Connection %p, Parsed Length String: '%s', Value: %lld\n", (void*)c, len_str, parsed_len);
                consume_bytes_from_buffer(c, len_str_len + 2); // Consuma numero + CRLF

                if (c->resp_type == '*') {
                    c->argc = (uint32_t)parsed_len;
                    c->current_arg_idx = 0;
                    DEBUG_PRINTF("DEBUG: Connection %p, RESP_TYPE is '*', argc: %u\n", (void*)c, c->argc);
                    if (c->argc > 0) {
                        c->argv = (char**)calloc(c->argc, sizeof(char*)); // calloc è più sicuro
                        if (!c->argv) { conn_error(c, "OOM"); return; }
                        c->parse_state = RESP_PARSE_TYPE;
                    }
                    else {
                        // Array vuoto *0\r\n. Questo è un comando valido.
                        DEBUG_PRINTF("DEBUG: Connection %p, Empty array ('*0\\r\\n'), command considered parsed.\n", (void*)c);
                        cmd_create_and_append(c, NULL, 0, &c->cmd_list_head, &c->cmd_list_tail); // Append an empty command
                        c->argc = 0;
                        c->current_arg_idx = 0;
                        c->parse_state = RESP_PARSE_TYPE; // Pronto per il prossimo comando
                        // return true; // Command complete (empty command)
                    }
                }
                else if (c->resp_type == '$') {
                    c->resp_expected_len = parsed_len;
                    DEBUG_PRINTF("DEBUG: Connection %p, RESP_TYPE is '$', expected_len: %lld\n", (void*)c, c->resp_expected_len);
                    if (c->resp_expected_len == -1) {
                        // Null bulk string
                        DEBUG_PRINTF("DEBUG: Connection %p, Null bulk string detected.\n", (void*)c);
                        // Null bulk string is treated as an argument
                        if (c->argv == NULL) { // This condition should ideally not be true if we're parsing an array's argument.
                            conn_error(c, "Unexpected null bulk string at top-level");
                            return;
                        }
                        if (c->current_arg_idx < c->argc) {
                            c->argv[c->current_arg_idx++] = NULL;
                        }
                        else {
                            conn_error(c, "Too many arguments for command (null bulk)");
                            return;
                        }
                        c->parse_state = RESP_PARSE_CRLF; // After null bulk, expect CRLF
                    }
                    else {
                        c->parse_state = RESP_PARSE_BULK_PAYLOAD;
                    }
                }
                else {
                    conn_error(c, "Unknown RESP type");
                    return;
                }
            }
            break;

        case RESP_PARSE_BULK_PAYLOAD:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_BULK_PAYLOAD, rbuf_size: %zu, expected_len: %lld\n", (void*)c, c->rbuf_size, c->resp_expected_len);
            if (c->rbuf_size < (size_t)c->resp_expected_len) {
                DEBUG_PRINTF("DEBUG: Connection %p, Insufficient data for bulk payload, returning false.\n", (void*)c);
                return; // Dati insufficienti
            }

            char* arg_str = (char*)malloc(c->resp_expected_len + 1);
            if (!arg_str) { conn_error(c, "OOM"); return; }
            memcpy(arg_str, c->rbuf, c->resp_expected_len);
            arg_str[c->resp_expected_len] = '\0';

            DEBUG_PRINTF("DEBUG: Connection %p, Parsed Bulk Payload: '%s'\n", (void*)c, arg_str);
            consume_bytes_from_buffer(c, c->resp_expected_len);
            DEBUG_PRINTF("DEBUG: Connection %p, Bulk payload consumed.\n", (void*)c);

            if (c->argv == NULL) { // Should have been allocated if c->argc > 0
                conn_error(c, "Internal error: argv not allocated before bulk payload");
                free(arg_str);
                return;
            }
            if (c->current_arg_idx < c->argc) {
                c->argv[c->current_arg_idx] = arg_str;
            }
            else {
                conn_error(c, "Too many arguments for command");
                free(arg_str);
                return;
            }

            DEBUG_PRINTF("DEBUG: Connection %p, argv[%d] set to '%s'\n", (void*)c, c->current_arg_idx, arg_str);
            c->parse_state = RESP_PARSE_CRLF;
            // Fallthrough

        case RESP_PARSE_CRLF:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_CRLF, rbuf_size: %zu\n", (void*)c, c->rbuf_size);
            if (c->rbuf_size < 2) {
                DEBUG_PRINTF("DEBUG: Connection %p, Insufficient data for CRLF, returning false.\n", (void*)c);
                return;
            }
            if (c->rbuf[0] != '\r' || c->rbuf[1] != '\n') {
                conn_error(c, "Bad CRLF"); return;
            }
            DEBUG_PRINTF("DEBUG: Connection %p, CRLF consumed.\n", (void*)c);
            consume_bytes_from_buffer(c, 2);
            c->current_arg_idx++;

            if (c->current_arg_idx == c->argc) {
                DEBUG_PRINTF("DEBUG: Connection %p, All arguments parsed, command complete.\n", (void*)c);
                c->parse_state = RESP_PARSE_DONE;
            }
            else {
                DEBUG_PRINTF("DEBUG: Connection %p, Parsing next argument (idx: %u/%u).\n", (void*)c, c->current_arg_idx, c->argc);
                c->parse_state = RESP_PARSE_TYPE; // Prossimo argomento
            }
            break;

        case RESP_PARSE_DONE:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_DONE. Command processed.\n", (void*)c);

            cmd_create_and_append(c, c->argv, c->argc, &c->cmd_list_head, &c->cmd_list_tail); // Append the parsed command to the connection's command list

            // Reset parsing-related fields for the next command
            c->argv = NULL; // ownership transferred to Command struct
            c->argc = 0;
            c->current_arg_idx = 0;
            c->parse_state = RESP_PARSE_TYPE;

            DEBUG_PRINTF("DEBUG: Connection %p, consume_buffer returning true after command processing.\n", (void*)c);
            // return;

        default:
            DEBUG_PRINTF("DEBUG: Connection %p, State: DEFAULT (Unknown state %d), resetting to RESP_PARSE_TYPE and returning false.\n", (void*)c, c->parse_state);
            c->parse_state = RESP_PARSE_TYPE;
            return;
        }
    }
}
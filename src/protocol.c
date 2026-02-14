#include "protocol.h"
#include "store.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <strings.h> // Required for strcasecmp

#ifdef DEBUG
#define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#endif

void free_argv(Conn* c) {
    if (c == NULL) return; // Un check silenzioso è più robusto dell'assert in produzione

    if (c->args) {
        DEBUG_PRINTF("DEBUG: Freeing args for connection %p\n", (void*)c);
        for (uint32_t i = 0; i < c->argc; ++i) {
            // Dobbiamo liberare il "contenuto" (data), non la struct stessa
            if (c->args[i].data != NULL) {
                free(c->args[i].data);   // <--- CORRETTO: libera il buffer dei dati
                c->args[i].data = NULL;
            }
        }
        // Una volta liberati tutti i contenuti, liberiamo l'intero array di struct
        free(c->args);
        c->args = NULL;
        c->argc = 0; // Buona pratica: resetta il conteggio
    }
}

static void conn_error(Conn* c, const char* msg);

// Crea un nuovo comando e lo appende alla lista specificata (head, tail).
// Restituisce il puntatore al nuovo comando in caso di successo, NULL altrimenti.
Command* cmd_create_and_append(Conn* c, Argument* args, uint32_t argc, Command** head, Command** tail) {
    DEBUG_PRINTF("DEBUG: Creating and appending command for connection %p: argc = %u\n", (void*)c, argc);
    Command* cmd = (Command*)malloc(sizeof(Command));
    if (!cmd) {
        conn_error(c, "OOM during command allocation");
        return NULL;
    }
    memset(cmd, 0, sizeof(Command));

    cmd->argc = argc;
    cmd->args = args; // Assume ownership of args
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

// Libera tutti gli argomenti dell'array di argomenti del comando e i suoi contenuti.
void command_free(Command* cmd) {
    if (cmd == NULL) return;

    if (cmd->args) {
        // 1. Libera ogni singolo buffer 'data' dentro l'array
        for (uint32_t i = 0; i < cmd->argc; i++) {
            if (cmd->args[i].data != NULL) {
                free(cmd->args[i].data);
                cmd->args[i].data = NULL; // Opzionale ma consigliato
            }
        }
        // 2. Ora che l'interno è vuoto, libera l'array di struct stesso
        free(cmd->args);
    }

    // 3. Infine libera la struttura Command stessa
    free(cmd);
}

// Libera tutti i comandi nella lista della connessione.
void free_command_list(Conn* c) {
    DEBUG_PRINTF("DEBUG: Freeing command list for connection %p\n", (void*)c);
    Command* cmd = c->cmd_list_head;
    while (cmd) {
        Command* next = cmd->next;
        command_free(cmd);
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
        // Questa funzione libera TUTTO (dati binari, array args e la struct cmd)
        command_free(cmd);
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

// Helper consigliato
int arg_is(Argument* arg, const char* name) {
    size_t name_len = strlen(name);
    return (arg->len == name_len) && (strncasecmp(arg->data, name, name_len) == 0);
}

// Esegue un singolo comando e restituisce la sua risposta in una stringa allocata dinamicamente.
// La stringa restituita deve essere liberata dal chiamante.
int get_command_reply(char* reply_buf, int max_len, Command* cmd, HashMap* store) {
    int len = 0;

    if (cmd->argc == 0 || cmd->args[0].data == NULL) {
        const char* reply_str = "OK";
        len = snprintf(reply_buf, max_len, "+%s\r\n", reply_str);
    }
    else if (arg_is(&cmd->args[0], "PING")) {
        if (cmd->argc > 1 && cmd->args[1].data != NULL) {
            char* message = cmd->args[1].data;
            size_t msg_len = cmd->args[1].len;

            // 1. Scriviamo il prefisso RESP (es. "$8\r\n")
            int header_len = snprintf(reply_buf, max_len, "$%zu\r\n", msg_len);

            // 2. Copiamo i dati binari "così come sono" con memcpy
            // (Assicurati che ci sia spazio nel reply_buf!)
            if (header_len + msg_len + 2 <= (size_t)max_len) {
                memcpy(reply_buf + header_len, message, msg_len);

                // 3. Aggiungiamo il CRLF finale richiesto dal protocollo
                memcpy(reply_buf + header_len + msg_len, "\r\n", 2);

                // Lunghezza totale effettiva della risposta
                len = header_len + msg_len + 2;
            }
            else {
                // Gestione errore buffer troppo piccolo
            }
        }
        else {
            // Caso PING senza argomenti -> "+PONG\r\n"
            len = snprintf(reply_buf, max_len, "+PONG\r\n");
        }
    }
    else if (arg_is(&cmd->args[0], "SET")) {
        if (cmd->argc != 3) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'set' command\r\n");
        }
        else {
            int set_result = store_set(store, &cmd->args[1], &cmd->args[2]);
            if (set_result == 1 || set_result == 0) { // 1 for new key, 0 for updated key
                const char* reply_str = "OK";
                len = snprintf(reply_buf, max_len, "+%s\r\n", reply_str);
            }
            else { // -1 for OOM or type mismatch error
                len = snprintf(reply_buf, max_len, "-ERR OOM during SET\r\n");
            }
        }
    }
    else if (arg_is(&cmd->args[0], "GET")) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'get' command\r\n");
        }
        else {
            struct Argument* value = store_get(store, &cmd->args[1]);
            if (value) {
                len = snprintf(reply_buf, max_len, "$%zu\r\n", value->len);
                memcpy(reply_buf + len, value->data, value->len);
                len += value->len;
                memcpy(reply_buf + len, "\r\n", 2);
                len += 2;
            }
            else {
                const char* reply_str = "-1";
                len = snprintf(reply_buf, max_len, "$%s\r\n", reply_str);
            }
        }
    }
    else if (arg_is(&cmd->args[0], "HSET")) {
        if (cmd->argc < 4 || (cmd->argc - 2) % 2 != 0) { // HSET key field value [field value ...] must have at least 4 args and an even number of field-value pairs
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'hset' command\r\n");
        }
        else {
            int new_fields_count = 0;
            // Iterate through field-value pairs. Start from cmd->args[2] as cmd->args[0] is HSET, cmd->args[1] is key.
            for (uint32_t i = 2; i < cmd->argc; i += 2) {
                int hset_result = store_hset(store, &cmd->args[1], &cmd->args[i], &cmd->args[i + 1]);
                if (hset_result == 1) { // New field added
                    new_fields_count++;
                }
                else if (hset_result == -1) { // Error during HSET (e.g., wrong type on key)
                    len = snprintf(reply_buf, max_len, "-ERR HSET failed: key is not a hash or other error\r\n");
                    // Break loop and return error immediately
                    new_fields_count = -1; // Indicate error
                    break;
                }
            }
            if (new_fields_count != -1) {
                len = snprintf(reply_buf, max_len, ":%d\r\n", new_fields_count);
            }
        }
    }
    else if (arg_is(&cmd->args[0], "HGET")) {
        if (cmd->argc != 3) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'hget' command\r\n");
        }
        else {
            struct Argument* value = store_hget(store, &cmd->args[1], &cmd->args[2]);
            if (value) {
                len = snprintf(reply_buf, max_len, "$%zu\r\n", value->len);
                memcpy(reply_buf + len, value->data, value->len);
                len += value->len;
                memcpy(reply_buf + len, "\r\n", 2);
                len += 2;
            }
            else {
                const char* reply_str = "-1";
                len = snprintf(reply_buf, max_len, "$%s\r\n", reply_str); // Nil bulk string
            }
        }
    }
    else if (arg_is(&cmd->args[0], "HLEN")) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'hlen' command\r\n");
        }
        else {
            int hlen_result = store_hlen(store, &cmd->args[1]);
            // hlen_result can be >= 0 (count) or -1 (error/not hash)
            // Redis HLEN returns 0 if key does not exist or is not a hash
            len = snprintf(reply_buf, max_len, ":%d\r\n", hlen_result);
        }
    }
    else if (arg_is(&cmd->args[0], "HDEL")) {
        if (cmd->argc < 3) { // HDEL key field [field ...] must have at least 3 args
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'hdel' command\r\n");
        }
        else {
            // cmd->args[1] is the key, cmd->args[2] onwards are fields
            // The fields array starts from cmd->args[2]
            // num_fields is cmd->argc - 2
            int hdel_result = store_hdel(store, &cmd->args[1], &cmd->args[2], cmd->argc - 2);
            if (hdel_result >= 0) { // Number of deleted fields (can be 0)
                len = snprintf(reply_buf, max_len, ":%d\r\n", hdel_result);
            }
            else if (hdel_result == -1) { // Wrong type
                len = snprintf(reply_buf, max_len, "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
            }
            else { // Other potential errors from store_hdel
                len = snprintf(reply_buf, max_len, "-ERR HDEL failed\r\n");
            }
        }
    }
    else if (arg_is(&cmd->args[0], "HGETALL")) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'hgetall' command\r\n");
        }
        else {
            struct Argument* results = NULL;
            size_t count = 0;
            int hgetall_status = store_hgetall(store, &cmd->args[1], &results, &count);

            if (hgetall_status == 0) { // Success
                len = snprintf(reply_buf, max_len, "*%zu\r\n", count);
                for (size_t i = 0; i < count; ++i) {
                    len += snprintf(reply_buf + len, max_len - len, "$%zu\r\n", results[i].len);
                    memcpy(reply_buf + len, results[i].data, results[i].len);
                    len += results[i].len;
                    memcpy(reply_buf + len, "\r\n", 2);
                    len += 2;
                    free(results[i].data);
                }
                free(results);
            }
            else if (hgetall_status == -1) { // Key not found or not a hash (wrong type)
                len = snprintf(reply_buf, max_len, "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
            }
        }
    }
    else if (arg_is(&cmd->args[0], "DEL")) {
        if (cmd->argc < 2) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'del' command\r\n");
        }
        else {
            // cmd->args[1] is the first key, cmd->argc - 1 is the number of keys
            int deleted_count = store_del(store, &cmd->args[1], cmd->argc - 1);
            len = snprintf(reply_buf, max_len, ":%d\r\n", deleted_count);
        }
    }
    else if (arg_is(&cmd->args[0], "EXISTS")) {
        if (cmd->argc != 2) { // Implementing for single key EXISTS for now
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'exists' command\r\n");
        }
        else {
            int exists_result = store_exists(store, &cmd->args[1]);
            len = snprintf(reply_buf, max_len, ":%d\r\n", exists_result);
        }
    }
    else if (arg_is(&cmd->args[0], "TYPE")) {
        if (cmd->argc != 2) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'type' command\r\n");
        }
        else {
            ObjType type = store_type(store, &cmd->args[1]);
            if (type == OBJ_STRING) {
                len = snprintf(reply_buf, max_len, "+string\r\n");
            }
            else if (type == OBJ_HASH) {
                len = snprintf(reply_buf, max_len, "+hash\r\n");
            }
            else { // (ObjType)-1 for not found
                len = snprintf(reply_buf, max_len, "+none\r\n");
            }
        }
    }
    else if (arg_is(&cmd->args[0], "FLUSHDB")) {
        if (cmd->argc != 1) {
            len = snprintf(reply_buf, max_len, "-ERR wrong number of arguments for 'flushdb' command\r\n");
        }
        else {
            store_flushdb(store);
            len = snprintf(reply_buf, max_len, "+OK\r\n");
        }
    }
    else {
        // Unknown command
        const char* reply_str = "OK"; // Default to OK for unknown commands
        len = snprintf(reply_buf, max_len, "+%s\r\n", reply_str);
    }

    if (len <= 0 || len >= max_len) {
        // Handle snprintf error or buffer overflow
        DEBUG_PRINTF("ERROR: snprintf failed or buffer overflow in get_command_reply.\n");
        return snprintf(reply_buf, max_len, "-ERR Internal server error\r\n");
    }

    return len;
}

void execute_command(Conn* c, HashMap* store) {
    DEBUG_PRINTF("DEBUG: execute_command called for connection %p\n", (void*)c);
    while (c->cmd_list_head != NULL) {
        Command* cmd = c->cmd_list_head;
        c->cmd_list_head = cmd->next;
        if (c->cmd_list_head == NULL) {
            c->cmd_list_tail = NULL; // List is now empty
        }

        char reply_buf[K_MAX_MSG * 2];
        int reply_len = 0;

        // Handle transaction commands
        if (cmd->argc > 0 && cmd->args[0].data != NULL) {
            if (arg_is(&cmd->args[0], "MULTI")) {
                if (c->in_transaction) {
                    reply_len = snprintf(reply_buf, sizeof(reply_buf), "-ERR MULTI already in progress\r\n");
                }
                else {
                    c->in_transaction = true;
                    reply_len = snprintf(reply_buf, sizeof(reply_buf), "+OK\r\n");
                }
            }
            else if (arg_is(&cmd->args[0], "EXEC")) {
                if (!c->in_transaction) {
                    reply_len = snprintf(reply_buf, sizeof(reply_buf), "-ERR EXEC without MULTI\r\n");
                }
                else {
                    // Execute all queued commands
                    c->in_transaction = false; // Transaction ends with EXEC
                    if (c->queued_cmds_head == NULL) {
                        reply_len = snprintf(reply_buf, sizeof(reply_buf), "*0\r\n"); // Empty array reply if no commands were queued
                    }
                    else {
                        int num_queued_cmds = 0;
                        Command* temp = c->queued_cmds_head;
                        while(temp) {
                            num_queued_cmds++;
                            temp = temp->next;
                        }

                        reply_len = snprintf(reply_buf, sizeof(reply_buf), "*%d\r\n", num_queued_cmds);
                        Command* current_queued_cmd = c->queued_cmds_head;
                        while (current_queued_cmd != NULL) {
                            reply_len += get_command_reply(reply_buf + reply_len, sizeof(reply_buf) - reply_len, current_queued_cmd, store);
                            current_queued_cmd = current_queued_cmd->next;
                        }
                    }
                    free_queued_command_list(c); // Clear the queued commands after EXEC
                }
            }
            else if (arg_is(&cmd->args[0], "DISCARD")) {
                if (!c->in_transaction) {
                    reply_len = snprintf(reply_buf, sizeof(reply_buf), "-ERR DISCARD without MULTI\r\n");
                }
                else {
                    c->in_transaction = false;
                    free_queued_command_list(c); // Discard queued commands
                    reply_len = snprintf(reply_buf, sizeof(reply_buf), "+OK\r\n");
                }
            }
            else if (c->in_transaction) {
                // If in transaction, queue the command
                cmd_create_and_append(c, cmd->args, cmd->argc, &c->queued_cmds_head, &c->queued_cmds_tail);
                // Important: clear cmd->args from the original cmd struct
                // so it's not freed twice later.
                cmd->args = NULL; // Ownership transferred to queued command.
                reply_len = snprintf(reply_buf, sizeof(reply_buf), "+QUEUED\r\n");
            }
        }

        // If not a transaction command or not in transaction, execute normally
        if (reply_len == 0) {
            reply_len = get_command_reply(reply_buf, sizeof(reply_buf), cmd, store);
        }

        if (reply_len > 0) {
            // Append reply to write buffer
            if (c->wbuf_size + reply_len < sizeof(c->wbuf)) {
                memcpy(c->wbuf + c->wbuf_size, reply_buf, reply_len);
                c->wbuf_size += reply_len;
            }
            else {
                DEBUG_PRINTF("ERROR: Write buffer overflow, dropping reply.\n");
                // In a real server, would need to handle this by flushing wbuf or resizing.
            }
        }

        // Free the command and its arguments (if not transferred to queued list)
        command_free(cmd);
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
                        c->args = calloc(c->argc, sizeof(Argument)); // calloc è più sicuro
                        if (!c->args) { conn_error(c, "OOM"); return; }
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
                        if (c->args == NULL) { // This condition should ideally not be true if we're parsing an array's argument.
                            conn_error(c, "Unexpected null bulk string at top-level");
                            return;
                        }
                        if (c->current_arg_idx < c->argc) {
                            // inizializziamo un Argument per il null bulk
                            Argument null_bulk_arg;
                            null_bulk_arg.len = 0;
                            null_bulk_arg.data = NULL;
                            c->args[c->current_arg_idx++] = null_bulk_arg;
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

            char* arg_str = (char*)malloc(c->resp_expected_len);
            if (!arg_str) { conn_error(c, "OOM"); return; }
            memcpy(arg_str, c->rbuf, c->resp_expected_len);

            DEBUG_PRINTF("DEBUG: Connection %p, Parsed Bulk Payload: '%.*s'\n", (void*)c, (int)c->resp_expected_len, arg_str);
            consume_bytes_from_buffer(c, c->resp_expected_len);
            DEBUG_PRINTF("DEBUG: Connection %p, Bulk payload consumed.\n", (void*)c);

            if (c->args == NULL) { // Should have been allocated if c->argc > 0
                conn_error(c, "Internal error: args not allocated before bulk payload");
                free(arg_str);
                return;
            }
            if (c->current_arg_idx < c->argc) {
                c->args[c->current_arg_idx].data = arg_str;
                c->args[c->current_arg_idx].len = c->resp_expected_len;
            }
            else {
                conn_error(c, "Too many arguments for command");
                free(arg_str);
                return;
            }

            DEBUG_PRINTF("DEBUG: Connection %p, args[%d] set to '%.*s'\n", (void*)c, c->current_arg_idx, (int)c->args[c->current_arg_idx].len, c->args[c->current_arg_idx].data);
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

            cmd_create_and_append(c, c->args, c->argc, &c->cmd_list_head, &c->cmd_list_tail); // Append the parsed command to the connection's command list

            // Reset parsing-related fields for the next command
            c->args = NULL; // ownership transferred to Command struct
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
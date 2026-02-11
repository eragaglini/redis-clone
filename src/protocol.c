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

// Appende un comando alla lista di comandi della connessione.
void cmd_append(Conn* c, char** argv, uint32_t argc) {
    DEBUG_PRINTF("DEBUG: Appending command to connection %p: argc = %u\n", (void*)c, argc);
    Command* cmd = (Command*)malloc(sizeof(Command));
    if (!cmd) {
        conn_error(c, "OOM during command allocation");
        return;
    }
    memset(cmd, 0, sizeof(Command));

    cmd->argc = argc;
    cmd->argv = argv; // Assume ownership of argv
    cmd->next = NULL;

    if (c->cmd_list_tail == NULL) {
        c->cmd_list_head = cmd;
        c->cmd_list_tail = cmd;
    }
    else {
        c->cmd_list_tail->next = cmd;
        c->cmd_list_tail = cmd;
    }
    DEBUG_PRINTF("DEBUG: Command appended. Head: %p, Tail: %p\n", (void*)c->cmd_list_head, (void*)c->cmd_list_tail);
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

// Esegue i comandi parsati e prepara le risposte.
void execute_command(Conn* c, HashMap* store) {
    DEBUG_PRINTF("DEBUG: execute_command called for connection %p\n", (void*)c);
    while (c->cmd_list_head != NULL) {
        Command* cmd = c->cmd_list_head;
        c->cmd_list_head = cmd->next;
        if (c->cmd_list_head == NULL) {
            c->cmd_list_tail = NULL; // List is now empty
        }

        // Print command arguments for debugging
        DEBUG_PRINTF("DEBUG: Executing command (argc: %u): ", cmd->argc);
        for (uint32_t i = 0; i < cmd->argc; ++i) {
            DEBUG_PRINTF("'%s'%s", cmd->argv[i] ? cmd->argv[i] : "(nil)", i == cmd->argc - 1 ? "" : ", ");
        }
        DEBUG_PRINTF("\n");

        if (cmd->argc == 0 || cmd->argv[0] == NULL) {
            const char* reply = "OK";
            int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "+%s\r\n", reply);
            if (len > 0) c->wbuf_size += (size_t)len;
            DEBUG_PRINTF("DEBUG: execute_command: Empty command, reply length: %d, wbuf_size: %zu\n", len, c->wbuf_size);
        }
        else if (strcasecmp(cmd->argv[0], "PING") == 0) {
            if (cmd->argc > 1 && cmd->argv[1] != NULL) {
                char* message = cmd->argv[1];
                int msg_len = strlen(message);
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "$%d\r\n%s\r\n", msg_len, message);
                if (len > 0) c->wbuf_size += (size_t)len;
            }
            else {
                const char* reply = "PONG";
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "+%s\r\n", reply); // Redis PONG is just +PONG\r\n
                if (len > 0) c->wbuf_size += (size_t)len;
            }
        }
        else if (strcasecmp(cmd->argv[0], "SET") == 0) {
            if (cmd->argc != 3) {
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "-ERR wrong number of arguments for 'set' command\r\n");
                if (len > 0) c->wbuf_size += (size_t)len;
            }
            else if (store_set(store, cmd->argv[1], cmd->argv[2])) {
                const char* reply = "OK";
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "+%s\r\n", reply);
                if (len > 0) c->wbuf_size += (size_t)len;
            }
            else {
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "-ERR OOM during SET\r\n");
                if (len > 0) c->wbuf_size += (size_t)len;
            }
        }
        else if (strcasecmp(cmd->argv[0], "GET") == 0) {
            if (cmd->argc != 2) {
                int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "-ERR wrong number of arguments for 'get' command\r\n");
                if (len > 0) c->wbuf_size += (size_t)len;
            }
            else {
                char* value = store_get(store, cmd->argv[1]);
                if (value) {
                    int val_len = strlen(value);
                    int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "$%d\r\n%s\r\n", val_len, value);
                    if (len > 0) c->wbuf_size += (size_t)len;
                    free(value); // Free the duplicated string from store_get
                }
                else {
                    // Key not found, return null bulk string
                    // Redis null bulk string is "$-1\r\n"
                    const char* reply_str = "-1";
                    int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "$%s\r\n", reply_str);
                    if (len > 0) c->wbuf_size += (size_t)len;
                }
            }
        }
        else {
            // Unknown command
            const char* reply = "OK";
            int len = snprintf((char*)c->wbuf + c->wbuf_size, sizeof(c->wbuf) - c->wbuf_size, "+%s\r\n", reply);
            if (len > 0) c->wbuf_size += (size_t)len;
        }

        // Free the command and its arguments
        if (cmd->argv) {
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
                        cmd_append(c, NULL, 0); // Append an empty command
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

            cmd_append(c, c->argv, c->argc); // Append the parsed command to the connection's command list

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
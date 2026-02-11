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
        for (uint32_t i = 0; i < c->current_arg_idx; ++i) {
            if (c->argv[i]) {
                free(c->argv[i]);
            }
        }
        free(c->argv);
        c->argv = NULL;
    }
}

static void conn_error(Conn* c, const char* msg) {
    c->error = 1;
    DEBUG_PRINTF("DEBUG: Connection error for %p: %s\n", (void*)c, msg);
    int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "-ERR %s\r\n", msg);
    if (len > 0) c->wbuf_size = (size_t)len;
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

// Esegue il comando parsato e prepara la risposta.
void execute_command(Conn* c, HashMap* store) {
    if (c->argc == 0 || c->argv[0] == NULL) {
        const char* reply = "OK"; 
        int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "+%s\r\n", reply);
        c->wbuf_size = (size_t)len;
        DEBUG_PRINTF("DEBUG: execute_command: Empty command, reply length: %d, wbuf_size: %zu\n", len, c->wbuf_size);
        return;
    }
    
    if (strcasecmp(c->argv[0], "PING") == 0) {
        if (c->argc > 1 && c->argv[1] != NULL) {
            char* message = c->argv[1];
            int msg_len = strlen(message);
            int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "$%d\r\n%s\r\n", msg_len, message);
            c->wbuf_size = (size_t)len;
        } else {
            const char* reply = "PONG";
            int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "+%s\r\n", reply); // Redis PONG is just +PONG\r\n
            c->wbuf_size = (size_t)len;
        }
    } else if (strcasecmp(c->argv[0], "SET") == 0) {
        if (c->argc != 3) {
            conn_error(c, "wrong number of arguments for 'set' command");
            return;
        }
        if (store_set(store, c->argv[1], c->argv[2])) {
            const char* reply = "OK";
            int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "+%s\r\n", reply);
            c->wbuf_size = (size_t)len;
        } else {
            conn_error(c, "OOM during SET");
        }
    } else if (strcasecmp(c->argv[0], "GET") == 0) {
        if (c->argc != 2) {
            conn_error(c, "wrong number of arguments for 'get' command");
            return;
        }
        char* value = store_get(store, c->argv[1]);
        if (value) {
            int val_len = strlen(value);
            int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "$%d\r\n%s\r\n", val_len, value);
            c->wbuf_size = (size_t)len;
            free(value); // Free the duplicated string from store_get
        } else {
            // Key not found, return null bulk string
            // Redis null bulk string is "$-1\r\n"
            const char* reply_str = "-1"; 
            int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "$%s\r\n", reply_str);
            c->wbuf_size = (size_t)len;
        }
    } else {
        // Unknown command
        const char* reply = "OK"; 
        int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "+%s\r\n", reply);
        c->wbuf_size = (size_t)len;
    }
}

bool consume_buffer(Conn* c) {
    DEBUG_PRINTF("DEBUG: consume_buffer called for connection %p, rbuf_size: %zu, parse_state: %d\n", (void*)c, c->rbuf_size, c->parse_state);

    if (c->error) {
        DEBUG_PRINTF("DEBUG: Connection %p in error state, clearing argv.\n", (void*)c);
        free_argv(c);
        return false; // Error state, no command fully parsed
    }

    while (1) {
        DEBUG_PRINTF("DEBUG: Connection %p, current parse_state: %d, rbuf_size: %zu\n", (void*)c, c->parse_state, c->rbuf_size);
        switch (c->parse_state) {
        case RESP_PARSE_TYPE:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_TYPE\n", (void*)c);
            // SE c'è un comando precedente completato, lo liberiamo ora
            // prima di iniziare a parsare quello nuovo.
            // Questa logica è stata spostata in RESP_PARSE_DONE per assicurare
            // che argv venga liberato solo dopo che un comando è stato
            // completamente elaborato.


            if (c->rbuf_size == 0) {
                DEBUG_PRINTF("DEBUG: Connection %p, rbuf_size is 0, returning false.\n", (void*)c);
                return false; // Attendi altri dati
            }
            c->resp_type = c->rbuf[0];
            DEBUG_PRINTF("DEBUG: Connection %p, Parsed RESP Type: %c\n", (void*)c, c->resp_type);
            consume_bytes_from_buffer(c, 1);
            c->parse_state = RESP_PARSE_LEN;
            // Fallthrough intenzionale

        case RESP_PARSE_LEN:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_LEN\n", (void*)c);
            { // blocco per variabili locali
                // Verifica l'esistenza di una sequenza completa di carriage return-line feed (CRLF) nel buffer rbuf.
                // Se la sequenza CRLF non è presente o incompleta, stampa un messaggio di debug e termina l'esecuzione della funzione.
                char* crlf = (char*)memchr(c->rbuf, '\r', c->rbuf_size);
                if (!crlf || crlf + 1 >= (char*)c->rbuf + c->rbuf_size || *(crlf + 1) != '\n') {
                    DEBUG_PRINTF("DEBUG: Connection %p, CRLF not found or incomplete, returning false.\n", (void*)c);
                    return false;
                }

                int len_str_len = crlf - (char*)c->rbuf;
                char len_str[32];
                if (len_str_len >= sizeof(len_str)) {
                    conn_error(c, "Length string too long");
                    return false;
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
                        if (!c->argv) { conn_error(c, "OOM"); return false; }
                        c->parse_state = RESP_PARSE_TYPE; 
                    } else {
                        // Array vuoto *0\r\n. Questo è un comando valido.
                        DEBUG_PRINTF("DEBUG: Connection %p, Empty array ('*0\\r\\n'), command considered parsed.\n", (void*)c);
                        c->wbuf_size = 0; // La risposta sarà gestita da execute_command
                        c->parse_state = RESP_PARSE_TYPE; // Pronto per il prossimo comando
                        return true; // Command complete (empty command)
                    }
                } else if (c->resp_type == '$') {
                    c->resp_expected_len = parsed_len;
                    DEBUG_PRINTF("DEBUG: Connection %p, RESP_TYPE is '$', expected_len: %lld\n", (void*)c, c->resp_expected_len);
                    if (c->resp_expected_len == -1) {
                        // Null bulk string
                         DEBUG_PRINTF("DEBUG: Connection %p, Null bulk string detected.\n", (void*)c);
                         if (c->argc > 0 && c->current_arg_idx < c->argc) {
                            c->argv[c->current_arg_idx++] = NULL;
                         }
                         c->parse_state = RESP_PARSE_TYPE; // Next arg or done
                         // Controllo se comando finito... (omesso per brevità, simile sotto)
                    } else {
                        c->parse_state = RESP_PARSE_BULK_PAYLOAD;
                    }
                } else {
                    conn_error(c, "Unknown RESP type");
                    return false;
                }
            }
            break;

        case RESP_PARSE_BULK_PAYLOAD:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_BULK_PAYLOAD, rbuf_size: %zu, expected_len: %lld\n", (void*)c, c->rbuf_size, c->resp_expected_len);
            if (c->rbuf_size < (size_t)c->resp_expected_len) {
                DEBUG_PRINTF("DEBUG: Connection %p, Insufficient data for bulk payload, returning false.\n", (void*)c);
                return false; // Dati insufficienti
            }
            
            char* arg_str = (char*)malloc(c->resp_expected_len + 1);
            if (!arg_str) { conn_error(c, "OOM"); return false; }
            memcpy(arg_str, c->rbuf, c->resp_expected_len);
            arg_str[c->resp_expected_len] = '\0';
            
            DEBUG_PRINTF("DEBUG: Connection %p, Parsed Bulk Payload: '%s'\n", (void*)c, arg_str);
            consume_bytes_from_buffer(c, c->resp_expected_len);
            DEBUG_PRINTF("DEBUG: Connection %p, Bulk payload consumed.\n", (void*)c);
            if (c->argv == NULL) {
            c->argv = (char**)calloc(c->argc, sizeof(char*));
                if (c->argv == NULL) {
                    conn_error(c, "OOM");
                    return false;
                }
                memset(c->argv, 0, c->argc * sizeof(char*));
            }
            c->argv[c->current_arg_idx] = arg_str;
            DEBUG_PRINTF("DEBUG: Connection %p, argv[%d] set to '%s'\n", (void*)c, c->current_arg_idx, arg_str);
            c->parse_state = RESP_PARSE_CRLF;
            // Fallthrough

        case RESP_PARSE_CRLF:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_CRLF, rbuf_size: %zu\n", (void*)c, c->rbuf_size);
            if (c->rbuf_size < 2) {
                DEBUG_PRINTF("DEBUG: Connection %p, Insufficient data for CRLF, returning false.\n", (void*)c);
                return false;
            }
            if (c->rbuf[0] != '\r' || c->rbuf[1] != '\n') {
                conn_error(c, "Bad CRLF"); return false;
            }
            DEBUG_PRINTF("DEBUG: Connection %p, CRLF consumed.\n", (void*)c);
            consume_bytes_from_buffer(c, 2);
            c->current_arg_idx++;

            if (c->current_arg_idx == c->argc) {
                DEBUG_PRINTF("DEBUG: Connection %p, All arguments parsed, command complete.\n", (void*)c);
                c->parse_state = RESP_PARSE_DONE;
            } else {
                DEBUG_PRINTF("DEBUG: Connection %p, Parsing next argument (idx: %u/%u).\n", (void*)c, c->current_arg_idx, c->argc);
                c->parse_state = RESP_PARSE_TYPE; // Prossimo argomento
            }
            break;

        case RESP_PARSE_DONE:
            DEBUG_PRINTF("DEBUG: Connection %p, State: RESP_PARSE_DONE. Command processed.\n", (void*)c);
            // Non prepariamo più la risposta qui; lo farà execute_command
            c->wbuf_size = 0; // Reset della dimensione del buffer di scrittura

            // Resetta lo stato per il prossimo comando.
            c->parse_state = RESP_PARSE_TYPE;
            
            // 3. IMPORTANTE: Return per dare il controllo al chiamante (Test o Event Loop)
            //    NON liberiamo argv qui. Lo farà il prossimo giro di RESP_PARSE_TYPE
            //    o il teardown del test.
            DEBUG_PRINTF("DEBUG: Connection %p, consume_buffer returning true after command processing.\n", (void*)c);
            return true; 

        default:
            DEBUG_PRINTF("DEBUG: Connection %p, State: DEFAULT (Unknown state %d), resetting to RESP_PARSE_TYPE and returning false.\n", (void*)c, c->parse_state);
            c->parse_state = RESP_PARSE_TYPE;
            return false;
        }
    }
}
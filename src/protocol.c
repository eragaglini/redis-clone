#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h> // Per malloc, free, abort
#include <assert.h> // Per la macro assert()

// Helper per liberare l'array di argomenti del comando e i suoi contenuti.
static void free_cmd_argv(Conn* c) {
    if (c->cmd_argv) {
        for (uint32_t i = 0; i < c->current_arg_idx; ++i) {
            free(c->cmd_argv[i]);
        }
        free(c->cmd_argv);
        c->cmd_argv = NULL;
    }
}

// Funzione di utilità per segnalare un errore di protocollo e preparare una risposta.
static void conn_error(Conn* c, const char* msg) {
    c->error = 1; // Segnala che questa connessione è in uno stato di errore e deve essere chiusa.
    // Prepara un messaggio di errore in formato Redis (es. "-ERR <msg>\r\n")
    // Se il buffer di scrittura è troppo piccolo, alza un'asserzione (dovrebbe essere abbastanza grande).
    int len = snprintf((char*)c->wbuf, sizeof(c->wbuf), "-ERR %s\r\n", msg);
    if (len < 0 || (size_t)len >= sizeof(c->wbuf)) {
        // Fallimento di snprintf o buffer troppo piccolo.
        // Questo è un errore grave: probabilmente non dovremmo abortire,
        // ma in questo contesto semplificato, lo facciamo per rilevare problemi di buffer.
        // In un server reale, ci sarebbe un meccanismo di log più robusto e si chiuderebbe la connessione.
        fprintf(stderr, "snprintf failed or wbuf too small for error message. len=%d, wbuf_size=%zu\n", len, sizeof(c->wbuf));
        abort();
    }
    c->wbuf_size = (size_t)len;
}

// Funzione di utilità per "consumare" (rimuovere) un certo numero di byte
// dall'inizio del buffer di lettura.
static void consume_bytes_from_buffer(Conn* c, size_t bytes) {
    assert(bytes <= c->rbuf_size);
    size_t remaining = c->rbuf_size - bytes;
    if (remaining > 0) {
        memmove(c->rbuf, &c->rbuf[bytes], remaining);
    }
    c->rbuf_size = remaining;
}

// La macchina a stati per il parsing dei comandi.
void consume_buffer(Conn* c) {
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] consume_buffer: Entered. parse_state = %d, rbuf_size = %zu, error = %d\n", c->parse_state, c->rbuf_size, c->error);
#endif
    if (c->error) {
        free_cmd_argv(c); // Cleanup any partial command arguments if an error occurred previously.
        return;
    }
    // Loop `while(1)`: La macchina a stati tenterà di avanzare attraverso
    // i suoi stati finché non ci saranno più dati sufficienti nel buffer.
    while (1) {
        switch (c->parse_state) {
        case STATE_PARSE_INIT:
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_INIT -> STATE_PARSE_NUM_ARGS\n");
#endif
            c->total_args_expected = 0;
            c->current_arg_idx = 0;
            c->arg_len = 0;
            c->cmd_argv = NULL;
            c->parse_state = STATE_PARSE_NUM_ARGS;
            // NOTA: Nessun 'break', passiamo subito a provare lo stato successivo.
        case STATE_PARSE_NUM_ARGS:
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_NUM_ARGS. rbuf_size = %zu\n", c->rbuf_size);
#endif
            if (c->rbuf_size < 4) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_NUM_ARGS - Insufficient data, returning.\n");
#endif
                return; // Dati insufficienti, attendiamo.
            }
            memcpy(&c->total_args_expected, c->rbuf, 4);
            consume_bytes_from_buffer(c, 4);
            // Validazione del numero di argomenti
            if (c->total_args_expected > MAX_COMMAND_ARGS) {
                conn_error(c, "Too many arguments");
                return; // Segnaliamo errore e chiudiamo la connessione.
            }
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: Parsed total_args_expected = %u\n", c->total_args_expected);
#endif
            if (c->total_args_expected > 0) {
                c->cmd_argv = (char**)malloc(c->total_args_expected * sizeof(char*));
                if (c->cmd_argv == NULL) {
                    conn_error(c, "Out of memory for cmd_argv"); // Graceful error instead of abort
                    return;
                }
                // Inizializza a NULL per sicurezza in caso di errori parziali.
                for (uint32_t i = 0; i < c->total_args_expected; ++i) {
                    c->cmd_argv[i] = NULL;

                }
            }
            if (c->total_args_expected == 0) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_NUM_ARGS - Empty command, resetting.\n");
#endif
                c->parse_state = STATE_PARSE_INIT; // Comando vuoto, ricomincia.

            }
            else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_NUM_ARGS -> STATE_PARSE_ARG_LEN\n");
#endif
                c->parse_state = STATE_PARSE_ARG_LEN;
            }
            break;
        case STATE_PARSE_ARG_LEN:
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_ARG_LEN. rbuf_size = %zu\n", c->rbuf_size);
#endif
            if (c->rbuf_size < 4) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_ARG_LEN - Insufficient data, returning.\n");
#endif
                return; // Dati insufficienti, attendiamo.
            }
            memcpy(&c->arg_len, c->rbuf, 4);
            consume_bytes_from_buffer(c, 4);
            // Validazione della lunghezza dell'argomento
            if (c->arg_len > MAX_ARG_LEN) {
                conn_error(c, "Argument too long");
                return; // Segnaliamo errore e chiudiamo la connessione.
            }
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: Parsed arg_len = %u. STATE_PARSE_ARG_LEN -> STATE_PARSE_ARG_PAYLOAD\n", c->arg_len);
#endif
            c->parse_state = STATE_PARSE_ARG_PAYLOAD;
            // NOTA: Nessun 'break', passiamo subito a provare lo stato successivo.

        case STATE_PARSE_ARG_PAYLOAD:
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_ARG_PAYLOAD. rbuf_size = %zu, expected arg_len = %u, current_arg_idx = %u\n", c->rbuf_size, c->arg_len, c->current_arg_idx);
#endif
            if (c->rbuf_size < c->arg_len) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: STATE_PARSE_ARG_PAYLOAD - Insufficient data for payload, returning.\n");
#endif
                return; // Dati insufficienti, attendiamo.
            }
            char* arg_str = (char*)malloc(c->arg_len + 1);
            if (arg_str == NULL) {
                conn_error(c, "Out of memory for arg_str"); // Graceful error instead of abort
                return;
            }
            memcpy(arg_str, c->rbuf, c->arg_len);
            arg_str[c->arg_len] = '\0';
            consume_bytes_from_buffer(c, c->arg_len);
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: Parsed arg '%s' (len %u) for index %u.\n", arg_str, c->arg_len, c->current_arg_idx);
#endif
            assert(c->current_arg_idx < c->total_args_expected);
            c->cmd_argv[c->current_arg_idx] = arg_str;
            c->current_arg_idx++;

            if (c->current_arg_idx == c->total_args_expected) {
                printf("Comando Parsato: ");
                for (uint32_t i = 0; i < c->total_args_expected; i++) {
                    printf("'%s' ", c->cmd_argv[i]);
                }
                printf("\n");
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: All arguments parsed. Command complete. Resetting to STATE_PARSE_INIT.\n");
#endif
                const char* reply = "OK";
                uint32_t reply_len = (uint32_t)strlen(reply);
                if (c->wbuf_size + 4 + reply_len <= sizeof(c->wbuf)) {
                    memcpy(&c->wbuf[c->wbuf_size], &reply_len, 4);
                    memcpy(&c->wbuf[c->wbuf_size + 4], reply, reply_len);
                    c->wbuf_size += 4 + reply_len;
                }

                free_cmd_argv(c); // This one is correct and should remain
                c->parse_state = STATE_PARSE_INIT;
            }
            else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] consume_buffer: More arguments expected. STATE_PARSE_ARG_PAYLOAD -> STATE_PARSE_ARG_LEN.\n");
#endif
                c->parse_state = STATE_PARSE_ARG_LEN;
            }
            break;

        default:
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] consume_buffer: Invalid parser state %d. Resetting to STATE_PARSE_INIT.\n", c->parse_state);
#endif
            fprintf(stderr, "Invalid parser state: %d\n", c->parse_state);
            c->parse_state = STATE_PARSE_INIT;
            return;
        }
        if (c->error) { // Check error flag after each state, if set, cleanup and exit consume_buffer.
            free_cmd_argv(c);
            return;
        }
    }
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] consume_buffer: Exiting. parse_state = %d, rbuf_size = %zu, error = %d\n", c->parse_state, c->rbuf_size, c->error);
#endif
}
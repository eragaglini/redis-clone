#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// La dimensione massima per il payload di un singolo messaggio (4KB).
#define K_MAX_MSG 4096

// Limiti del protocollo per prevenire attacchi DoS o consumo eccessivo di risorse.
#define MAX_COMMAND_ARGS 1024 // Numero massimo di argomenti in un comando.
#define MAX_ARG_LEN K_MAX_MSG // Lunghezza massima di un singolo argomento.

// Definisce i vari stati in cui il nostro parser si può trovare
// mentre analizza un comando in arrivo. Questo è il cuore della nostra macchina a stati.
typedef enum {
    STATE_PARSE_INIT,          // Stato iniziale: in attesa di un nuovo comando.
    STATE_PARSE_NUM_ARGS,      // In attesa di leggere il numero di argomenti del comando.
    STATE_PARSE_ARG_LEN,       // In attesa di leggere la lunghezza del prossimo argomento.
    STATE_PARSE_ARG_PAYLOAD,   // In attesa di leggere il corpo (payload) dell'argomento.
} ParseState;

// La struttura dati che rappresenta lo stato di una singola connessione client.
typedef struct {
    int fd;

    // Buffer di lettura
    size_t rbuf_size;
    uint8_t rbuf[4 + K_MAX_MSG];

    // Buffer di scrittura
    size_t wbuf_size;
    size_t wbuf_sent;
    uint8_t wbuf[4 + K_MAX_MSG];

    // --- Stato per il parser della macchina a stati ---
    ParseState parse_state;           // Lo stato attuale del parser per questa connessione.
    uint32_t total_args_expected; // Il numero totale di argomenti per il comando corrente.
    uint32_t current_arg_idx;     // L'indice dell'argomento che stiamo leggendo/salvando ora.
    uint32_t arg_len;             // La lunghezza dell'argomento che stiamo leggendo ora.
    char** cmd_argv;              // L'array di stringhe (es. {"SET", "key", "value"})

    // Flag di errore per la connessione. Se settato, la connessione deve essere chiusa.
    int error;

    // Ultimo timestamp di attività (millisecondi dall'avvio del server), per gestione timeout.
    uint64_t last_activity_time;

} Conn;

// Funzione "pura" per il parsing del buffer e la preparazione della risposta.
void consume_buffer(Conn* c);

#endif // PROTOCOL_H

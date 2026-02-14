#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h> // Include for bool type
#include <stdlib.h> // For malloc/free

// Forward declaration
struct HashMap;

// La dimensione massima per il payload di un singolo messaggio (4KB).
#define K_MAX_MSG 4096

// Limiti del protocollo per prevenire attacchi DoS o consumo eccessivo di risorse.
#define MAX_COMMAND_ARGS 1024 // Numero massimo di argomenti in un comando.
#define MAX_ARG_LEN K_MAX_MSG // Lunghezza massima di un singolo argomento.

// Definisce i vari stati in cui il nostro parser RESP si può trovare
// mentre analizza un comando in arrivo. Questo è il cuore della nostra macchina a stati.
typedef enum {
    RESP_PARSE_TYPE,           // In attesa di leggere il tipo di dato RESP (es. *, $, +)
    RESP_PARSE_LEN,            // In attesa di leggere la lunghezza (es. per bulk stringhe o array)
    RESP_PARSE_CRLF,           // In attesa di leggere CR LF dopo lunghezza o stringa semplice
    RESP_PARSE_BULK_PAYLOAD,   // In attesa di leggere il payload della stringa bulk
    RESP_PARSE_DONE,           // Parsing del comando completato
    RESP_PARSE_ERROR,          // Stato di errore del parser
} ParseState;

// Singolo blocco di dati binari
typedef struct Argument {
    char* data;    // Buffer allocato dinamicamente
    size_t len;    // Lunghezza effettiva (può includere byte nulli)
} Argument;

// Comando completo (es. SET key value -> argc=3)
typedef struct Command {
    Argument* args;       // Array di struct Argument
    uint32_t argc;        // Numero di argomenti
    struct Command* next; // Per gestire il pipelining (lista di comandi)
} Command;

// La struttura dati che rappresenta lo stato di una singola connessione client.
typedef struct Conn {
    int fd;

    // Buffer di lettura
    size_t rbuf_size;
    uint8_t rbuf[4 + K_MAX_MSG];

    // Buffer di scrittura
    size_t wbuf_size;
    size_t wbuf_sent;
    uint8_t wbuf[4 + K_MAX_MSG];

    // --- Stato per il parser della macchina a stati RESP ---
    ParseState parse_state;
    char resp_type;                 // '*' per array, '$' per bulk string
    long long resp_expected_len;    
    long long resp_current_offset;  
    
    // CAMBIO CHIAVE: Da char** a Argument*
    Argument* args;                 // Array di struct Argument allocato dinamicamente
    uint32_t argc;                  // Numero totale di argomenti (da *N)
    uint32_t current_arg_idx;       // Indice dell'argomento che stiamo riempiendo

    int error;

    // Ultimo timestamp di attività (millisecondi dall'avvio del server), per gestione timeout.
    uint64_t last_activity_time;

    // Liste di comandi (Pipelining)
    Command* cmd_list_head;
    Command* cmd_list_tail;

    // --- Transaction State ---
    bool in_transaction;
    Command* queued_cmds_head;
    Command* queued_cmds_tail;

} Conn;

// Crea un nuovo comando e lo appende alla lista specificata (head, tail).
// Restituisce il puntatore al nuovo comando in caso di successo, NULL altrimenti.
Command* cmd_create_and_append(Conn* c, struct Argument* args, uint32_t argc, Command** head, Command** tail);

// Funzione per il parsing del buffer e la preparazione della risposta.
void consume_buffer(Conn* c);

// Helper per liberare l'array di argomenti del comando e i suoi contenuti.
void free_args(Conn* c);

// Libera tutti i comandi nella lista della connessione.
void free_command_list(Conn* c);

// Libera tutti i comandi nella lista di comandi accodati per la transazione.
void free_queued_command_list(Conn* c);

// Esegue il comando parsato e prepara la risposta.
void execute_command(Conn* c, struct HashMap* store, int from_aof);

// Esegue un singolo comando e restituisce la sua risposta in una stringa allocata dinamicamente.
int get_command_reply(char* reply_buf, int max_len, Command* cmd, struct HashMap* store);
#endif // PROTOCOL_H
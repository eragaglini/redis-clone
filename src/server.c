#include "server.h"
#include "protocol.h" // Per la definizione di Conn e la funzione consume_buffer

// Librerie standard e di sistema
// Queste librerie forniscono funzionalità essenziali per lo sviluppo di applicazioni di sistema e di rete in C.
#include <assert.h>      // Per la macro assert(), utile per rilevare condizioni inaspettate in fase di debug.
#include <errno.h>       // Per la variabile globale `errno` che indica l'ultimo errore di sistema.
#include <fcntl.h>       // Per la funzione `fcntl()` che permette di manipolare i file descriptor, come impostarli non bloccanti.
#include <netinet/ip.h>  // Definizioni per il protocollo Internet (IP), inclusi tipi di indirizzi e strutture.
#include <poll.h>        // Per la funzione `poll()` e la `struct pollfd`, cruciali per l'I/O multiplexing non bloccante.
#include <stdio.h>       // Funzioni di Input/Output standard come `printf`, `fprintf`, `perror`.
#include <stdlib.h>      // Funzioni di utilità generale, inclusa l'allocazione dinamica di memoria (`malloc`, `free`, `abort`).
#include <string.h>      // Funzioni per la manipolazione di stringhe e blocchi di memoria (`memcpy`, `memmove`, `strlen`).
#include <sys/socket.h>  // Le API fondamentali per la programmazione dei socket (`socket`, `bind`, `listen`, `accept`, `send`, `recv`).
#include <unistd.h>      // Funzioni POSIX come `close()` per chiudere i file descriptor.
#include <time.h>        // Per clock_gettime, struct timespec

// =====================================================================================
// COSTANTI DI CONFIGURAZIONE DEL SERVER
// =====================================================================================
// `MAX_CONN` definisce il numero massimo di connessioni client che il server può gestire
// contemporaneamente. Questo limita la dimensione degli array `fd2conn` e `poll_args`.
#define MAX_CONN 1000
#define TIMEOUT_MS 5000 // Timeout per poll() in millisecondi (5 secondi)
#define CONNECTION_TIMEOUT_MS 10000 // Timeout per connessioni inattive (10 secondi)

// =====================================================================================
// STATO GLOBALE DEL SERVER (STATICO AL MODULO server.c)
// =====================================================================================
// Queste variabili sono dichiarate `static` per limitarne la visibilità al solo file `server.c`.
// Questo impedisce ad altri file (`main.c`, `protocol.c`) di accedervi direttamente,
// incapsulando lo stato del server all'interno del suo modulo.
//
// CONSIDERAZIONI SUL DESIGN:
// - Vantaggi delle globali `static`: Semplicità d'uso all'interno del modulo, non c'è bisogno
//   di passare esplicitamente lo stato tra le funzioni interne.
// - Svantaggi: Rende più difficile l'espansione (es. se si volessero più istanze del server
//   nello stesso processo) e limita la flessibilità.
// - Alternativa: Incapsulare questo stato in una `struct Server` (es. `struct ServerState { Conn *fd2conn[MAX_CONN]; struct pollfd poll_args[MAX_CONN]; ... }`)
//   e passare un puntatore a questa struct a tutte le funzioni che ne hanno bisogno.
//   Questo è l'approccio preferito in un software più grande e modulare.
//
// `fd2conn`: Un array che mappa i file descriptor dei socket client alle rispettive
// strutture `Conn`. Quando `poll()` ci dice che un `fd` ha un evento, possiamo
// accedere rapidamente allo stato (`Conn`) di quel client usando `fd2conn[fd]`.
// NOTA IMPORTANTE: I file descriptor (fd) sono interi assegnati dal kernel. Possono
// assumere valori molto più grandi di `MAX_CONN` (es. 1024, 65535, ecc.). Usare un array
// come mappa diretta (`fd2conn[fd]`) è una semplificazione comune per piccoli server,
// ma può portare a sprechi di memoria o a "segmentation fault" se `fd` supera `MAX_CONN`.
// In un server di produzione, si userebbe una hash map (`std::map` in C++, o implementazioni
// C come `uthash`) o un array dinamico e compattato (gestendo un mapping interno).
// static Conn *fd2conn[MAX_CONN] = {0}; // rimosso a favore di 'connections'
static Conn* connections[MAX_CONN] = { 0 }; // Mappa l'indice dell'array poll_args alla struct Conn.

// `poll_args`: L'array di `struct pollfd` che viene passato alla funzione di sistema `poll()`.
// `poll()` monitora gli eventi su ciascun file descriptor listato in questo array.
static struct pollfd poll_args[MAX_CONN] = { 0 }; // Inizializzato a zero.


// =====================================================================================
// FUNZIONI DI UTILITÀ GENERALI E GESTIONE ERRORI
// =====================================================================================

// Stampa un messaggio informativo o di errore sullo standard error (`stderr`).
// `stderr` è un canale separato da `stdout`, usato tipicamente per i messaggi di log.
static void msg(const char* msg_text) {
    fprintf(stderr, "%s\n", msg_text);
}

// Gestisce gli errori critici. Stampa il messaggio fornito insieme all'errore
// di sistema corrente (`errno`), poi termina brutalmente il programma con `abort()`.
// `abort()` è preferibile a `exit()` in caso di errori gravi, poiché genera
// un core dump (su sistemi Unix-like) utile per il debugging e l'analisi post-mortem.
static void die(const char* msg_text) {
    int err = errno; // Salviamo `errno` perché le chiamate seguenti potrebbero modificarlo.
    fprintf(stderr, "[%d] %s\n", err, msg_text);
    abort();
}

// Restituisce il tempo corrente in millisecondi (tempo monotono).
// Questo è utile per calcolare i timeout senza essere influenzati dai cambiamenti
// dell'ora di sistema (es. NTP che corregge l'orologio).
static uint64_t get_monotonic_time_ms() {
    struct timespec ts;
    // CLOCK_MONOTONIC_RAW è preferibile a CLOCK_MONOTONIC per una maggiore robustezza
    // contro le regolazioni dell'orologio (anche se CLOCK_MONOTONIC è più comune).
    // Se CLOCK_MONOTONIC_RAW non è disponibile, CLOCK_MONOTONIC è una buona alternativa.
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == -1) {
        // Fallback a CLOCK_MONOTONIC se RAW non è disponibile o in caso di errore
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
            die("clock_gettime failed"); // Errore grave se anche CLOCK_MONOTONIC fallisce.
        }
    }
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Imposta un file descriptor (socket) in modalità NON BLOCCANTE.
// Questa è una tecnica fondamentale per la programmazione di server event-driven.
// In modalità bloccante (default), operazioni come `read()`, `write()`, `accept()`
// aspetterebbero indefinitamente fino all'arrivo di dati/connessioni, bloccando l'intero programma.
// In modalità non bloccante, queste chiamate ritornano immediatamente, con un errore
// speciale (EAGAIN o EWOULDBLOCK) se non c'è nulla da fare. Questo permette al nostro
// event loop di continuare a girare e servire altri client.
static void fd_set_nb(int fd) {
    errno = 0; // Azzera `errno` prima della chiamata di sistema `fcntl()`.

    // 1. `fcntl(fd, F_GETFL, 0)`: Legge i flag di stato attuali del file descriptor `fd`.
    //    `F_GETFL` è un comando per `fcntl` che recupera i flag di accesso e di stato del file.
    //    È importante leggere i flag esistenti per non sovrascrivere altre impostazioni importanti.
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) { // Controlla se `fcntl` ha fallito nel leggere i flag.
        die("fcntl error (GETFL)");
    }

    // 2. `flags |= O_NONBLOCK;`: Aggiunge il flag `O_NONBLOCK` usando l'operatore OR bit-a-bit.
    //    `O_NONBLOCK` è il flag che indica al kernel che le operazioni di I/O su questo fd
    //    non devono bloccare il processo.
    flags |= O_NONBLOCK;

    // 3. `fcntl(fd, F_SETFL, flags)`: Scrive i nuovi flag aggiornati nel file descriptor.
    //    `F_SETFL` è un comando per `fcntl` che imposta i flag di accesso e di stato del file.
    //    Da questo punto, tutte le operazioni di I/O su `fd` saranno non bloccanti.
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags); // `(void)` cast per sopprimere un avviso del compilatore se il valore di ritorno non viene usato.
    if (errno) { // Controlla se `fcntl` ha fallito nel settare i flag.
        die("fcntl error (SETFL)");
    }
}

// Chiude una connessione client e libera le risorse associate.
// Questa funzione è cruciale per prevenire memory leak e per gestire
// la disconnessione dei client in modo pulito.
static void conn_close(int pfd_idx) { // Ora accetta l'indice dell'array poll_args.
    Conn* c = connections[pfd_idx];
    if (!c) { // Controllo di sicurezza, non dovrebbe mai accadere.
        return;
    }
    printf("Client FD %d disconnesso\n", c->fd);
    close(c->fd); // Chiude il socket, liberando il file descriptor nel kernel.
    // Rimuove la connessione dalla mappa globale e dall'array di pollfd.
    connections[pfd_idx] = NULL; // Imposta a NULL nello slot corretto.
    poll_args[pfd_idx].fd = -1; // Invalida lo slot, indicando che è libero.
    poll_args[pfd_idx].events = 0; // Azzera gli eventi.
    poll_args[pfd_idx].revents = 0; // Azzera i revents.

    // Libera la memoria allocata per la struct Conn.
    free(c);
}

// =====================================================================================
// FUNZIONI DI GESTIONE DEL CICLO DI VITA DELLA CONNESSIONE E I/O
// =====================================================================================

// Inizializza una `struct pollfd` per una data connessione `Conn` e la aggiunge all'array `poll_args`.
// Questa funzione viene usata per dire a `poll()` quali eventi ci interessano per un dato socket.
static void conn_put(struct pollfd* pfd, Conn* c) {
    pfd->fd = c->fd; // Il file descriptor del socket client da monitorare.
    // `events`: Specifica i tipi di eventi per cui vogliamo essere notificati.
    // `POLLIN`: Indica che vogliamo essere avvisati quando ci sono dati da leggere sul socket.
    // `POLLOUT`: Indica che vogliamo essere avvisati quando possiamo scrivere dati sul socket
    //             senza bloccarsi (es. il buffer di invio del kernel ha spazio disponibile).
    pfd->events = POLLIN | POLLOUT;
    pfd->revents = 0; // `revents` è un campo di output che viene riempito da `poll()` con gli eventi
    // effettivamente verificatisi. Lo azzeriamo prima di ogni chiamata a `poll()`.
}

// Gestisce l'accettazione di nuove connessioni in arrivo sul socket di ascolto.
// Questa funzione viene chiamata quando `poll()` segnala un evento `POLLIN` sul `listen_fd`.
static void handle_new_connection(int listen_fd) {
    while (1) {
        struct sockaddr_in client_addr = {};
        socklen_t socklen = sizeof(client_addr);
        int connfd = accept(listen_fd, (struct sockaddr*)&client_addr, &socklen);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("accept");
            return;
        }

        // Cerca uno slot disponibile nell'array `poll_args` e `connections`.
        int j = -1;
        for (int i = 1; i < MAX_CONN; i++) {
            if (poll_args[i].fd == -1) { // -1 in poll_args[i].fd indica uno slot libero.
                j = i;
                break;
            }
        }

        if (j == -1) { // Nessuno slot libero disponibile.
            close(connfd); // Chiudiamo subito la connessione in arrivo.
            fprintf(stderr, "Raggiunto il limite di slot (MAX_CONN) per FD %d. Connessione chiusa.\n", connfd);
            continue; // Continua il loop per accettare altre connessioni valide se presenti.
        }

        fd_set_nb(connfd); // Imposta il nuovo client socket a non bloccante.

        Conn* c = (Conn*)malloc(sizeof(Conn)); // Alloca dinamicamente memoria per la `Conn` struct.
        if (!c) { // Gestione del fallimento di `malloc` (memoria esaurita).
            close(connfd);
            fprintf(stderr, "malloc fallito per Conn struct per FD %d. Connessione chiusa.\n", connfd);
            continue; // Continua il loop.
        }

        // Inizializzazione pulita della nuova struct Conn.
        c->fd = connfd;
        c->rbuf_size = 0; c->wbuf_size = 0; c->wbuf_sent = 0;
        c->parse_state = STATE_PARSE_INIT; c->total_args_expected = 0; c->current_arg_idx = 0;
        c->arg_len = 0; c->cmd_argv = NULL; c->error = 0;
        c->last_activity_time = get_monotonic_time_ms();

        connections[j] = c; // Mappa la `Conn` struct allo slot `j`.
        conn_put(&poll_args[j], c); // Inizializza lo slot `pollfd` per il client.

        printf("Nuova connessione accettata: FD %d, slot %d\n", connfd, j);
    }
}

// Gestisce l'Input/Output (lettura e scrittura) per un client esistente.
// Questa è la funzione orchestratore che viene chiamata quando `poll()`
// segnala eventi su un socket client. Il suo ruolo è delegare i compiti:
// leggere, far processare la logica, e scrivere.
static void handle_client_io(int pfd_idx) {
    int connfd = poll_args[pfd_idx].fd; // Il file descriptor del client per cui si è verificato un evento.
    Conn* c = connections[pfd_idx]; // Recupera lo stato (`Conn` struct) di questa connessione.
    if (!c) return; // Controllo di sicurezza: la connessione potrebbe essere stata chiusa altrove.

    // --- FASE DI LETTURA ---
    // Controlla se l'evento `POLLIN` (dati da leggere) si è verificato per questo socket.
    if (poll_args[pfd_idx].revents & POLLIN) {
        // `read()` tenta di leggere dati dal socket. Poiché il socket è non bloccante,
        // questa chiamata ritornerà immediatamente.
                    // I dati vengono accodati al buffer di lettura (`c->rbuf`), a partire da `c->rbuf_size`.
                    // `sizeof(c->rbuf) - c->rbuf_size` è lo spazio disponibile nel buffer.
        ssize_t n_read = read(c->fd, &c->rbuf[c->rbuf_size], sizeof(c->rbuf) - c->rbuf_size);
        if (n_read > 0) { // Se `read()` ha letto uno o più byte.
            c->rbuf_size += (size_t)n_read; // Aggiorna la dimensione totale dei dati validi nel buffer.
            printf("Ricevuti %zd bytes dal FD %d\n", n_read, connfd);
            c->last_activity_time = get_monotonic_time_ms(); // Aggiorna l'orario di ultima attività.

            // Passa il controllo alla funzione `consume_buffer` (definita in `protocol.c`).
            // Questa funzione si occupa della "logica pura" di parsing dei messaggi e
            // di generazione delle risposte, operando solo sui buffer della `Conn` struct.
            consume_buffer(c);
        }

        // Gestione della disconnessione o errori di lettura.
        // `read()` ritorna 0 se il client ha chiuso la connessione (End Of File - EOF).
        // `read()` ritorna < 0 in caso di errore.
        if (n_read <= 0 || c->error) { // Aggiungiamo il controllo c->error per errori di protocollo
            // `n_read < 0` e `errno` è `EAGAIN` o `EWOULDBLOCK`:
            // Questo indica che non ci sono dati disponibili *al momento* per leggere.
            // Non è un errore, ma il normale comportamento di un socket non bloccante.
            if (n_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !c->error) {
                // Il server deve solo riprovare a leggere più tardi, quando `poll()` segnalerà `POLLIN` di nuovo.
            }
            else {
                // Qui gestiamo la chiusura della connessione (sia per `n_read == 0` che per errori gravi).
                conn_close(pfd_idx); // Usa la funzione centralizzata di chiusura.
                return;       // La connessione non esiste più, quindi usciamo da questa funzione.
            }
        }
    }

    // --- FASE DI SCRITTURA ---
    // Controlla se l'evento `POLLOUT` (il socket è pronto per scrivere) si è verificato
    // E se ci sono effettivamente dati da inviare nel buffer di scrittura (`c->wbuf`).
    if (poll_args[pfd_idx].revents & POLLOUT) {
        if (c->wbuf_size > c->wbuf_sent) { // Ci sono dati nel buffer di scrittura (`c->wbuf`) da inviare?
            // `write()` tenta di inviare i dati. Poiché il socket è non bloccante,
            // `write()` potrebbe non inviare tutti i byte in una sola chiamata (write parziale).
            ssize_t n_written = write(connfd, &c->wbuf[c->wbuf_sent], c->wbuf_size - c->wbuf_sent);

            if (n_written > 0) { // Se `write()` ha inviato uno o più byte.
                c->wbuf_sent += n_written; // Aggiorna il contatore dei byte inviati con successo.
                c->last_activity_time = get_monotonic_time_ms(); // Aggiorna l'orario di ultima attività.
            }

            // Se tutti i dati nel buffer di scrittura (`c->wbuf`) sono stati inviati.
            if (c->wbuf_sent == c->wbuf_size) {
                c->wbuf_size = 0;   // Resetta la dimensione del buffer di scrittura, è ora vuoto.
                c->wbuf_sent = 0; // Resetta il contatore dei byte inviati.
                // CONSIDERAZIONE: In un server più avanzato, potremmo voler disattivare il monitoraggio
                // di `POLLOUT` in `poll_args[i].events` quando `wbuf` è vuoto per ridurre il carico
                // su `poll()` e riattivarlo solo quando ci sono nuovi dati da inviare.
            }

            // Gestione errori di scrittura.
            if (n_written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Il buffer di invio del kernel è pieno. Non è un errore, ma un'indicazione
                // che `write()` non può accettare altri dati al momento.
                // `poll()` ci notificherà di nuovo con `POLLOUT` quando sarà pronto.
            }
            else if (n_written < 0) {
                // Errore reale di scrittura, trattato come disconnessione del client.
                conn_close(pfd_idx); // Usa la funzione centralizzata di chiusura.
            }
        }
    }
}

// =====================================================================================
// FUNZIONE PRINCIPALE DI AVVIO E GESTIONE DEL SERVER
// =====================================================================================
// Questa funzione è il cuore del modulo `server.c`. Viene chiamata da `main.c`
// per avviare e gestire l'intero processo del server.
void server_run(void) {
    // ---------- FASE 1: SETUP DEL SOCKET DI ASCOLTO (LISTENING SOCKET) ----------

    // 1. `socket()`: Crea un nuovo endpoint di comunicazione (il socket).
    //    Restituisce un file descriptor intero che rappresenta questo socket.
    //    `AF_INET`: Specifica la famiglia di indirizzi (IPv4).
    //    `SOCK_STREAM`: Specifica il tipo di socket orientato alla connessione (TCP).
    //    `0`: Protocollo (lascia al sistema di scegliere il default per SOCK_STREAM, che è TCP).
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket()"); // `die()` termina se il socket non può essere creato.

    // 2. `setsockopt()`: Imposta opzioni sul socket.
    //    `SO_REUSEADDR`: Questa opzione è CRUCIALE in fase di sviluppo e per server che si riavviano spesso.
    //    Permette al server di riavviarsi immediatamente sulla stessa porta dopo essere stato chiuso,
    //    senza dover aspettare che il sistema operativo rilasci completamente la porta
    //    (il periodo di attesa nello stato TCP `TIME_WAIT`). Senza questa opzione, dopo la chiusura,
    //    potrebbe essere necessario attendere alcuni minuti prima di poter riutilizzare la porta.
    int val = 1; // Valore per abilitare l'opzione (`1` per abilitare, `0` per disabilitare).
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // 3. `bind()`: Associa il socket creato a un indirizzo IP e a un numero di porta specifici.
    //    In questo modo, il sistema operativo sa che il nostro server vuole ricevere
    //    connessioni dirette a quell'indirizzo e porta.
    struct sockaddr_in addr = {}; // Inizializza la struct a zero per evitare valori "spazzatura".
    addr.sin_family = AF_INET;     // Famiglia di indirizzi (IPv4).
    addr.sin_port = ntohs(1234);   // Porta in cui il server si metterà in ascolto (qui: 1234).
    // `ntohs()` (Network To Host Short): Converte l'ordine dei byte
    // della porta da "network byte order" (big-endian) a "host byte order"
    // (che può essere little-endian o big-endian a seconda dell'architettura).
    // Questo garantisce la compatibilità di rete.
    addr.sin_addr.s_addr = ntohl(0); // Indirizzo IP su cui ascoltare.
    // `0.0.0.0` (rappresentato da 0) significa "ascolta su tutte
    // le interfacce di rete disponibili sul sistema" (es. Ethernet, Wi-Fi, Loopback).
    // `ntohl()` (Network To Host Long): Converte l'ordine dei byte per l'IP.
    if (bind(listen_fd, (const struct sockaddr*)&addr, sizeof(addr))) die("bind()");

    // 4. `listen()`: Mette il socket in modalità passiva (ascolto).
    //    Indica che il socket è pronto ad accettare connessioni in ingresso.
    //    `SOMAXCONN`: Questo parametro definisce la lunghezza massima della coda di
    //    connessioni in attesa che sono state ricevute dal kernel ma non ancora
    //    accettate dal nostro server con `accept()`.
    if (listen(listen_fd, SOMAXCONN)) die("listen()");

    // 5. Impostiamo il socket di ascolto come NON BLOCCANTE.
    // Questo è fondamentale per l'architettura event-driven.
    // `accept()` su questo socket non bloccherà il server in attesa di connessioni,
    // ma ritornerà `EAGAIN` se non ci sono nuove connessioni.
    fd_set_nb(listen_fd);

    // 6. Inizializzazione dell'array `poll_args` per la funzione `poll()`.
    // Questo array comunica a `poll()` quali file descriptor monitorare.
    // Lo slot 0 è sempre riservato al socket di ascolto (`listen_fd`).
    poll_args[0].fd = listen_fd;
    poll_args[0].events = POLLIN; // Per il socket di ascolto, ci interessa solo
    // quando ci sono nuove connessioni da accettare (evento `POLLIN`).
// Inizializziamo gli altri slot dell'array `poll_args` con `-1`. Un `fd` di `-1`
// indica a `poll()` di ignorare quello slot.
    for (int i = 1; i < MAX_CONN; i++) {
        poll_args[i].fd = -1;
    }

    msg("Server avviato. In attesa di connessioni sulla porta 1234...");

    // ---------- FASE 2: L'EVENT LOOP PRINCIPALE ----------
    // Questo è il cuore pulsante di un server event-driven e non bloccante.
    // Invece di dedicare un thread separato o bloccare l'esecuzione per ogni singola connessione,
    // questo loop monitora efficientemente tutti i socket attivi (sia di ascolto che dei client)
    // e reagisce solo quando si verifica un evento di I/O su uno di essi.
    while (1) {
        // `poll()`: Questa è la chiamata di sistema centrale che abilita l'I/O multiplexing.
        //   - `poll_args`: L'array di `struct pollfd` che contiene tutti i socket da monitorare.
        //   - `MAX_CONN`: Il numero di elementi nell'array `poll_args` da considerare.
        //   - `-1`: Timeout in millisecondi. `-1` significa che `poll()` si bloccherà
        //           indefinitamente finché non si verifica almeno un evento su un socket monitorato.
        // Restituisce il numero di file descriptor per cui si sono verificati eventi.
        int num_events = poll(poll_args, MAX_CONN, TIMEOUT_MS);
        if (num_events < 0) { // Gestione errori di `poll()`.
            die("poll()");
        }

        // Se `poll()` ritorna con 0 eventi, significa che è scaduto il timeout.
        // Dobbiamo controllare le connessioni inattive.
        if (num_events == 0) {
            uint64_t now = get_monotonic_time_ms();
            for (int i = 1; i < MAX_CONN; i++) {
                Conn* c = connections[i]; // Get the connection object for this slot
                if (c && (now - c->last_activity_time > CONNECTION_TIMEOUT_MS)) {
                    printf("Client FD %d inattivo per troppo tempo. Chiudo connessione.\n", c->fd);
                    conn_close(i); // Use the centralized closing function with index.
                }
            }
        }

        // Una volta che `poll()` ritorna (cioè, si è verificato almeno un evento),
        // scorriamo l'array `poll_args` per identificare quali socket hanno eventi
        // e cosa è successo (`revents`).

        // CASO A: Controlliamo il socket di ascolto (slot 0) per nuove connessioni.
        // Se `poll_args[0].revents` contiene `POLLIN`, significa che ci sono
        // nuove connessioni in attesa di essere accettate.
        if (poll_args[0].revents & POLLIN) {
            handle_new_connection(listen_fd); // Chiamiamo la funzione per accettare le nuove connessioni.
        }

        // CASO B: Controlliamo tutti gli altri slot (da 1 a MAX_CONN-1)
        // che corrispondono a client già connessi.
        for (int i = 1; i < MAX_CONN; i++) {
            // Condizioni per elaborare lo slot:
            // 1. `poll_args[i].fd != -1`: Assicura che lo slot sia effettivamente in uso da un client.
            // 2. `poll_args[i].revents`: Verifica se ci sono eventi su questo socket (non è zero).
            // Aggiungiamo anche il controllo `connections[i]->error` qui,
            // così da chiudere immediatamente le connessioni in errore segnalate da protocol.c.
            if (poll_args[i].fd != -1) { // Only process active slots
                Conn* c = connections[i]; // Get the connection object for this slot
                // If there are events or a protocol error, handle it.
                if (poll_args[i].revents || (c && c->error)) {
                    // If there's a protocol error signaled by protocol.c, close the connection.
                    if (c && c->error) {
                        printf("Client FD %d ha segnalato un errore di protocollo. Chiudo connessione.\n", c->fd);
                        conn_close(i); // Use the centralized closing function with index.
                        continue; // Skip to next connection.
                    }
                    // Call the function that handles I/O (read/write) for this client.
                    handle_client_io(i); // Pass the index.
                }
            }
        }
    }
    // `return 0;`: Questa riga non verrà mai raggiunta, dato che l'event loop è infinito.
    // Viene mantenuta la riga commentata per ricordare che `server_run` non dovrebbe mai terminare.
    // return 0;
}

# Redis Clone in C

Questo è un semplice server TCP scritto in C, progettato come un clone basilare di Redis per scopi didattici. Lo scopo principale di questo progetto è dimostrare e imparare i concetti fondamentali della programmazione di rete e di sistema in un ambiente Unix-like.

## Caratteristiche Principali

*   **Implementazione Protocollo RESP (Redis Serialization Protocol):** Il server ora interpreta una versione semplificata del protocollo RESP di Redis, permettendo l'interazione con client standard (es. `redis-py`).
*   **Supporto Comandi Basilari:** Supporto iniziale per il comando `PING`, con una chiara separazione tra parsing del comando e logica di esecuzione.
*   **Mitigazioni DOS:** Include meccanismi per prevenire Denial of Service attraverso la validazione dei limiti del protocollo e l'implementazione di timeout per le connessioni inattive.
*   **Architettura Non Bloccante:** Utilizza I/O non bloccante per gestire più client simultaneamente su un singolo thread.
*   **Multiplexing con `poll()`:** Usa la chiamata di sistema `poll()` per monitorare in modo efficiente più socket (sia di ascolto che dei client).
*   **Gestione dei Buffer:** Implementa una gestione manuale dei buffer di lettura e scrittura per gestire i messaggi in streaming.
*   **Test Unitari:** Include una suite di test unitari scritti con il framework [CMocka](https://cmocka.org/) per testare la logica di parsing dei messaggi.
*   **Makefile Configurabile:** Fornisce un `Makefile` semplice per compilare il progetto, eseguire i test e abilitare una modalità di debug.

## Come Iniziare

### Prerequisiti

*   Un compilatore C (es. `gcc` o `clang`)
*   `make`
*   `git` (per clonare il repository)
*   Python 3 (per eseguire il client di test)

### Compilazione

1.  **Clona il repository:**
    ```bash
    git clone <URL_DEL_TUO_REPOSITORY>
    cd <NOME_DELLA_CARTELLA>
    ```

2.  **Compila il server:**
    Per compilare l'eseguibile principale, esegui semplicemente `make`.
    ```bash
    make
    ```
    Questo creerà l'eseguibile in `bin/main`.

### Esecuzione

*   **Avviare il Server:**
    ```bash
    ./bin/main
    ```
    Il server si metterà in ascolto sulla porta 1234.

*   **Avviare il Client di Test:**
    Il client Python fornito è stato aggiornato per utilizzare la libreria standard `redis-py`.
    
    1.  **Crea e attiva un ambiente virtuale (consigliato):**
        ```bash
        python3 -m venv .venv
        source .venv/bin/activate
        ```
    2.  **Installa la libreria `redis-py`:**
        ```bash
        pip install redis
        ```
    3.  **Avvia il client:**
        ```bash
        python3 client.py
        ```
    Assicurati che il server C sia in esecuzione in un terminale separato.

### Testing

Il progetto usa CMocka per i test unitari.

*   **Compilare ed Eseguire i Test:**
    ```bash
    make test
    ```
    Questo comando compilerà ed eseguirà tutti i test definiti in `tests/main_test.c`.

### Modalità di Debug

È possibile compilare il server in modalità di debug per abilitare log aggiuntivi.

1.  **Pulisci le build precedenti:**
    ```bash
    make clean
    ```
2.  **Compila in modalità debug:**
    ```bash
    make DEBUG=1
    ```
3.  **Avvia il server:**
    ```bash
    ./bin/main
    ```

## Struttura del Progetto

```
.
├── bin/              # Eseguibili compilati (main server, run_tests)
├── lib/              # Librerie di terze parti (CMocka)
├── src/
│   └── main.c        # Codice sorgente principale del server
│   └── server.c      # Implementazione del server TCP e event loop
│   └── protocol.c    # Logica di parsing del protocollo
│   └── protocol.h    # Definizioni del protocollo e della struttura Conn
│   └── store.c       # Implementazione della struttura dati del Key-Value store
│   └── store.h       # Dichiarazione della struttura dati del Key-Value store
├── tests/
│   └── main_test.c   # Unit test per la logica del protocollo
├── client.py         # Semplice client Python per testare il server (implementa il protocollo)
├── Makefile          # Regole per la compilazione e l'esecuzione
├── .gitignore        # File e directory da ignorare con git
├── README.md         # Questo file
└── GEMINI.md         # File di contesto per l'agente Gemini CLI
```

## Contesto per Gemini CLI (`GEMINI.md`)

Il file `GEMINI.md` contiene una panoramica dettagliata del progetto, la sua architettura e i componenti principali, generata dall'agente Gemini CLI. Questo file viene utilizzato dall'agente per mantenere il contesto attraverso le sessioni, fornendo una base di conoscenza immediata per successive interazioni e compiti. Sebbene sia principalmente per uso dell'agente, può servire anche come documentazione di alto livello per gli sviluppatori.

## Limitazioni Attuali e Lavoro Futuro

Questo progetto è un clone rudimentale di Redis e, come tale, presenta diverse limitazioni e aree di sviluppo futuro:

*   **Implementazione Parziale di Key-Value Store:** Il server ora supporta il parsing del comando `PING` e lo esegue correttamente. L'architettura per aggiungere altri comandi è stata impostata con la funzione `execute_command`. I comandi `SET` e `GET` non hanno ancora effetti reali sulla memoria, ma la struttura per un Key-Value store è stata definita (vedi `src/store.h`).
*   **Set di Comandi Limitato:** Attualmente è implementato solo il comando `PING`. Altri comandi standard di Redis (come `SET`, `GET`, `DEL`, ecc.) devono ancora essere implementati in `execute_command`.
*   **Gestione Errori Protocollo:** Sebbene siano state implementate mitigazioni DoS e la gestione degli errori di protocollo sia più robusta, le risposte di errore ai client sono ancora generiche ("-ERR ..."). Una gestione più dettagliata e specifica degli errori sarebbe desiderabile.
*   **Scalabilità e Robustezza:** Per un utilizzo in produzione, sarebbero necessarie ulteriori ottimizzazioni per la scalabilità (es. thread pool, epoll/kqueue) e una gestione degli errori più granulare (es. non abortire per errori non critici).
*   **Mancanza di Persistenza:** Il server non salva i dati su disco, quindi tutti i dati vengono persi al riavvio.
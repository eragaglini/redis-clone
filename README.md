# Redis Clone in C

Questo è un semplice server TCP scritto in C, progettato come un clone basilare di Redis per scopi didattici. Lo scopo principale di questo progetto è dimostrare e imparare i concetti fondamentali della programmazione di rete e di sistema in un ambiente Unix-like.

## Caratteristiche Principali

*   **Binary-Safe Protocol Implementation:** Il protocollo RESP è ora completamente binary-safe, il che significa che il server può gestire correttamente qualsiasi sequenza di byte nei valori delle chiavi e nei nomi dei campi hash, inclusi i byte nulli. Questo garantisce la compatibilità con le specifiche di Redis e permette di memorizzare dati binari arbitrari.
*   **Implementazione Protocollo RESP (Redis Serialization Protocol):** Il server ora interpreta una versione semplificata del protocollo RESP di Redis, permettendo l'interazione con client standard (es. `redis-py`).
*   **Supporto Comandi Basilari, Transazioni, Tipi Hash e Comandi Generici per Chiavi:** Supporto per i comandi `PING`, `SET`, `GET`, `HSET` (con campi multipli), `HGET`, `HLEN`, `HDEL` (con campi multipli), `HGETALL` e per i comandi generici per chiavi `DEL`, `EXISTS`, `TYPE` e `FLUSHDB`. Include anche le transazioni con `MULTI`/`EXEC`/`DISCARD`. La separazione tra parsing del comando e logica di esecuzione è chiara.
*   **Pipelining Completo:** Il server gestisce correttamente il pipelining di comandi, elaborando più comandi inviati in una singola richiesta senza problemi di blocco.
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



### Testing (Unitari e di Integrazione)

Il progetto usa CMocka per i test unitari e Pytest per i test di integrazione.

*   **Test Unitari (C):**
    *   **Compilare ed Eseguire i Test:**
        ```bash
        make test
        ```
        Questo comando compilerà ed eseguirà tutti i test unitari C definiti in `tests/main_test.c`.

*   **Test di Integrazione (Python con Pytest):**
    *   **Prerequisiti:**
        Assicurati di avere `pytest` e `redis` installati nel tuo ambiente Python. Puoi installarli via `pip`:
        ```bash
        pip install -r requirements.txt
        ```
    *   **Compilare ed Eseguire i Test:**
        ```bash
        make integration_test
        ```
        Questo comando compilerà il server C e poi eseguirà i test di integrazione Python definiti in `tests/test_integration.py`. Il server C verrà automaticamente avviato e fermato come parte del processo di test.

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

## Documentazione Doxygen

Il progetto include la documentazione del codice generata automaticamente con Doxygen.

*   **Generare la Documentazione:**
    ```bash
    make doc
    ```
    Questo comando genererà i file HTML della documentazione nella directory `html/`. Per visualizzarla, apri `html/index.html` nel tuo browser.
    Nota: Per la generazione dei grafici (call graph, include graph, etc.), è necessario avere installato Graphviz (`dot` command).

## Struttura del Progetto

```
.
├── bin/              # Eseguibili compilati (main server, run_tests per i test unitari C)
├── lib/              # Librerie di terze parti (CMocka)
├── src/
│   └── main.c        # Codice sorgente principale del server
│   └── mainpage.dox  # Pagina principale della documentazione Doxygen
│   └── server.c      # Implementazione del server TCP e event loop
│   └── protocol.c    # Logica di parsing del protocollo
│   └── protocol.h    # Definizioni del protocollo e della struttura Conn
│   └── store.c       # Implementazione della struttura dati del Key-Value store
│   └── store.h       # Dichiarazione della struttura dati del Key-Value store
├── tests/
│   └── main_test.c   # Unit test C per la logica del protocollo
│   └── test_integration.py # Test di integrazione Python con Pytest
├── requirements.txt  # Dipendenze Python (pytest, redis-py)
├── Makefile          # Regole per la compilazione, l'esecuzione e i test
├── .gitignore        # File e directory da ignorare con git
├── Doxyfile          # File di configurazione per Doxygen
├── README.md         # Questo file
└── GEMINI.md         # File di contesto per l'agente Gemini CLI
```

## Contesto per Gemini CLI (`GEMINI.md`)

Il file `GEMINI.md` contiene una panoramica dettagliata del progetto, la sua architettura e i componenti principali, generata dall'agente Gemini CLI. Questo file viene utilizzato dall'agente per mantenere il contesto attraverso le sessioni, fornendo una base di conoscenza immediata per successive interazioni e compiti. Sebbene sia principalmente per uso dell'agente, può servire anche come documentazione di alto livello per gli sviluppatori.

## Limitazioni Attuali e Lavoro Futuro

Questo progetto è un clone rudimentale di Redis e, come tale, presenta diverse limitazioni e aree di sviluppo futuro:

*   **Implementazione di Key-Value Store:** Il server ora supporta i comandi `PING`, `SET`, `GET` per i tipi stringa, `HSET`, `HGET`, `HLEN`, `HDEL`, `HGETALL` per i tipi hash, e `DEL`, `EXISTS`, `TYPE` per la gestione generica delle chiavi, che interagiscono con una hash map in-memory. L'architettura per aggiungere altri comandi è stata impostata con la funzione `execute_command`. I dati vengono immagazzinati e recuperati correttamente. Il server include anche il supporto per le transazioni `MULTI`/`EXEC`/`DISCARD`.
*   **Set di Comandi Limitato:** Attualmente sono implementati solo i comandi `PING`, `SET`, `GET`, `HSET` (con campi multipli), `HGET`, `HLEN`, `HDEL` (con campi multipli), `HGETALL`, `DEL`, `EXISTS`, `TYPE`, `MULTI`, `EXEC`, `DISCARD` e `FLUSHDB`. Altri comandi standard di Redis (es. `LPUSH`, `SADD`) devono ancora essere implementati in `execute_command`.
*   **Gestione Errori Protocollo:** Sebbene siano state implementate mitigazioni DoS e la gestione degli errori di protocollo sia più robusta, le risposte di errore ai client sono ancora generiche ("-ERR ..."). Una gestione più dettagliata e specifica degli errori sarebbe desiderabile.
*   **Scalabilità e Robustezza:** Per un utilizzo in produzione, sarebbero necessarie ulteriori ottimizzazioni per la scalabilità (es. thread pool, epoll/kqueue) e una gestione degli errori più granular (es. non abortire per errori non critici).
*   **Mancanza di Persistenza:** Il server non salva i dati su disco, quindi tutti i dati vengono persi al riavvio.
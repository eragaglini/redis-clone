# Redis Clone in C

Questo è un semplice server TCP scritto in C, progettato come un clone basilare di Redis per scopi didattici. Lo scopo principale di questo progetto è dimostrare e imparare i concetti fondamentali della programmazione di rete e di sistema in un ambiente Unix-like.

## Caratteristiche Principali

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
    In un'altra finestra del terminale, puoi usare il client Python fornito per inviare messaggi al server:
    ```bash
    python3 client.py
    ```

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
├── bin/              # Eseguibili compilati (ignorato da git)
├── include/          # File di intestazione (attualmente vuoto)
├── lib/              # Librerie di terze parti (CMocka)
├── src/
│   └── main.c        # Codice sorgente principale del server
├── tests/
│   └── main_test.c   # Unit test per il server
├── client.py         # Semplice client Python per testare il server
├── Makefile          # Regole per la compilazione e l'esecuzione
├── .gitignore        # File e directory da ignorare con git
└── README.md         # Questo file
```

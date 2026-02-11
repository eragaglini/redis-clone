#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cmocka.h>

#include "protocol.h"
#include "store.h" // Include for HashMap and store functions

// --- SETUP & TEARDOWN ---

// Struttura per passare più oggetti ai test
typedef struct {
    Conn* conn;
    HashMap* store;
} TestState;

static int setup(void** state) {
    TestState* test_state = (TestState*)calloc(1, sizeof(TestState));
    assert_non_null(test_state);

    test_state->conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(test_state->conn);
    test_state->conn->parse_state = RESP_PARSE_TYPE;
    test_state->conn->cmd_list_head = NULL;
    test_state->conn->cmd_list_tail = NULL;

    test_state->store = (HashMap*)calloc(1, sizeof(HashMap));
    assert_non_null(test_state->store);
    store_init(test_state->store);

    *state = test_state;
    return 0;
}


static int teardown(void** state) {
    TestState* test_state = (TestState*)*state;
    if (test_state) {
        if (test_state->conn) {
            free_command_list(test_state->conn); // Pulisce la lista di comandi
            free(test_state->conn);      // Pulisce la struttura Conn
        }
        if (test_state->store) {
            store_free(test_state->store); // Libera tutti gli elementi dello store
            free(test_state->store);       // Libera la struttura HashMap
        }
        free(test_state); // Libera la struttura TestState
    }
    return 0;
}

// --- HELPER ---

static void build_fake_command(uint8_t* buffer, size_t buffer_size, size_t* size, int num_args, ...) {
    va_list args;
    va_start(args, num_args);
    *size = 0;
    int n = snprintf((char*)buffer + *size, buffer_size - *size, "*%d\r\n", num_args);
    *size += n;
    for (int i = 0; i < num_args; i++) {
        const char* arg = va_arg(args, const char*);
        uint32_t len = (uint32_t)strlen(arg);
        n = snprintf((char*)buffer + *size, buffer_size - *size, "$%u\r\n", len);
        *size += n;
        memcpy(buffer + *size, arg, len);
        *size += len;
        n = snprintf((char*)buffer + *size, buffer_size - *size, "\r\n");
        *size += n;
    }
    va_end(args);
}

// --- TEST CASES ---

// Test 1: Comando intero
static void test_parse_full_command_at_once(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    uint8_t buf[K_MAX_MSG];
    size_t size;
    build_fake_command(buf, K_MAX_MSG, &size, 3, "SET", "key", "val");

    memcpy(conn->rbuf, buf, size);
    conn->rbuf_size = size;

    // Parsa il comando e lo accoda
    consume_buffer(conn);

    // Verifica che un comando sia stato accodato
    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;

    // Verifiche sul comando accodato
    assert_int_equal(cmd->argc, 3);
    assert_string_equal(cmd->argv[0], "SET");
    assert_string_equal(cmd->argv[1], "key");
    assert_string_equal(cmd->argv[2], "val");
    assert_null(cmd->next); // Dovrebbe esserci un solo comando

    // Esegui il comando per generare la risposta
    execute_command(conn, test_state->store);

    // Verifica che la lista dei comandi sia ora vuota
    assert_null(conn->cmd_list_head);
    assert_null(conn->cmd_list_tail);

    // Verifica la risposta generata
    assert_int_equal(conn->wbuf_size, 5); // Expected response "+OK\r\n" has length 5.
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
}

// Test 2: Comando a pezzi
static void test_parse_command_in_chunks(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    uint8_t full_buf[K_MAX_MSG];
    size_t full_size;
    build_fake_command(full_buf, K_MAX_MSG, &full_size, 2, "GET", "a_key");

    // Chunk 1: "*2\r\n$3\r\n" (8 byte)
    memcpy(conn->rbuf, full_buf, 8);
    conn->rbuf_size = 8;
    consume_buffer(conn);
    assert_int_equal(conn->parse_state, RESP_PARSE_BULK_PAYLOAD); // After reading '*', expecting length of array
    assert_int_equal(conn->rbuf_size, 0); // Consumato tutto
    assert_null(conn->cmd_list_head); // Nessun comando ancora completo

    // Chunk 2: "GET\r\n" (5 byte)
    memcpy(conn->rbuf, full_buf + 8, 5);
    conn->rbuf_size = 5;
    consume_buffer(conn);
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE);
    assert_null(conn->cmd_list_head);

    // Chunk 3: "$5\r\n" (4 byte)
    memcpy(conn->rbuf, full_buf + 13, 4);
    conn->rbuf_size = 4;
    consume_buffer(conn);
    assert_int_equal(conn->parse_state, RESP_PARSE_BULK_PAYLOAD); // Expecting payload
    assert_null(conn->cmd_list_head);

    // Chunk 4: "a_key\r\n" (7 byte)
    memcpy(conn->rbuf, full_buf + 17, 7);
    conn->rbuf_size = 7;
    consume_buffer(conn); // This call should complete the command

    // Un comando completo dovrebbe essere nella lista
    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;

    // Verifiche sul comando accodato
    assert_int_equal(cmd->argc, 2);
    assert_string_equal(cmd->argv[0], "GET");
    assert_string_equal(cmd->argv[1], "a_key");
    assert_null(cmd->next);

    // Esegui il comando per generare la risposta
    execute_command(conn, test_state->store);

    // Verifica che la lista dei comandi sia ora vuota
    assert_null(conn->cmd_list_head);
    assert_null(conn->cmd_list_tail);

    // Verifica la risposta generata (GET di una chiave inesistente)
    assert_int_equal(conn->wbuf_size, 5); // Expected response "$-1\r\n" has length 5.
    assert_string_equal((char*)conn->wbuf, "$-1\r\n");
}

// Test 3: Due comandi in un solo buffer (Pipelining)
static void test_parse_two_commands_at_once(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    uint8_t buf1[K_MAX_MSG];
    size_t size1;
    build_fake_command(buf1, K_MAX_MSG, &size1, 1, "PING");

    uint8_t buf2[K_MAX_MSG];
    size_t size2;
    build_fake_command(buf2, K_MAX_MSG, &size2, 1, "PONG");

    // Mettiamo entrambi nel buffer di lettura
    memcpy(conn->rbuf, buf1, size1);
    memcpy(conn->rbuf + size1, buf2, size2);
    conn->rbuf_size = size1 + size2;

    // 1. Parsa il primo comando (PING)
    consume_buffer(conn);

    // Verifica che il primo comando sia nella coda
    assert_non_null(conn->cmd_list_head);
    assert_string_equal(conn->cmd_list_head->argv[0], "PING");
    assert_null(conn->cmd_list_head->next); // Solo un comando accodato per ora
    assert_int_equal(conn->rbuf_size, size2); // Deve rimanere PONG nel buffer
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE);

    // 2. Parsa il secondo comando (PONG)
    consume_buffer(conn);

    // Verifica che il secondo comando sia accodato
    assert_non_null(conn->cmd_list_head->next); // Ora ci sono due comandi
    assert_string_equal(conn->cmd_list_head->next->argv[0], "PONG");
    assert_int_equal(conn->rbuf_size, 0); // Buffer vuoto

    // Esegui entrambi i comandi
    execute_command(conn, test_state->store);

    // Verifica che la lista dei comandi sia ora vuota
    assert_null(conn->cmd_list_head);
    assert_null(conn->cmd_list_tail);

    // Verifica le risposte combinate
    // PING -> +PONG\r\n (5 bytes)
    // PONG -> +OK\r\n   (5 bytes) -- assuming PONG is not a special command handled with custom response here
    // Original logic: PONG replies with +OK\r\n for unknown command.
    assert_int_equal(conn->wbuf_size, 12);
    assert_string_equal((char*)conn->wbuf, "+PONG\r\n+OK\r\n");
}

// Test 4: Comando array vuoto (es. Heartbeat custom o errore client)
static void test_parse_empty_command(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    HashMap* store = test_state->store;

    // *0\r\n
    char* empty_cmd_str = "*0\r\n";
    size_t len = strlen(empty_cmd_str);

    memcpy(conn->rbuf, (uint8_t*)empty_cmd_str, len);
    conn->rbuf_size = len;

    // Chiamiamo consume_buffer per parsare il comando
    consume_buffer(conn);

    // Verifica che un comando vuoto sia stato accodato
    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;
    assert_int_equal(cmd->argc, 0);
    assert_null(cmd->argv);
    assert_null(cmd->next);

    // Eseguiamo il comando per generare la risposta
    execute_command(conn, store);

    // Verifica che la lista dei comandi sia ora vuota
    assert_null(conn->cmd_list_head);
    assert_null(conn->cmd_list_tail);

    // Verifichiamo che la risposta sia stata generata
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE); // Pronto per il prossimo comando
    assert_int_equal(conn->wbuf_size, 5); // Expected response "+OK\r\n" has length 5.
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
}

// --- MAIN ---

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_parse_full_command_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_command_in_chunks, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_two_commands_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_empty_command, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
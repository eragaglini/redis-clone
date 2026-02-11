#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cmocka.h>

#include "protocol.h"

// --- SETUP & TEARDOWN ---

static int setup(void** state) {
    Conn* conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(conn);
    conn->parse_state = RESP_PARSE_TYPE;
    *state = conn;
    return 0;
}


static int teardown(void** state) {
    Conn* conn = (Conn*)*state;
    if (conn) {
        free_argv(conn); // Pulisce argv se esiste
        free(conn);      // Pulisce la struttura
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
    Conn* conn = (Conn*)*state;

    uint8_t buf[K_MAX_MSG];
    size_t size;
    build_fake_command(buf, K_MAX_MSG, &size, 3, "SET", "key", "val");

    memcpy(conn->rbuf, buf, size);
    conn->rbuf_size = size;

    consume_buffer(conn);

    // Verifiche
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE); // Pronto per il prossimo
    assert_non_null(conn->argv); // I dati ci devono essere!
    assert_int_equal(conn->argc, 3);
    assert_string_equal(conn->argv[0], "SET");
    assert_string_equal(conn->argv[1], "key");
    assert_string_equal(conn->argv[2], "val");

    // Niente free() qui! Ci pensa teardown.
}

// Test 2: Comando a pezzi
static void test_parse_command_in_chunks(void** state) {
    Conn* conn = (Conn*)*state;
    uint8_t full_buf[K_MAX_MSG];
    size_t full_size;
    build_fake_command(full_buf, K_MAX_MSG, &full_size, 2, "GET", "a_key");

    // Chunk 1: "*2\r\n$3\r\n" (8 byte)
    memcpy(conn->rbuf, full_buf, 8);
    conn->rbuf_size = 8;
    consume_buffer(conn);

    // Deve essere in attesa del payload di GET
    assert_int_equal(conn->parse_state, RESP_PARSE_BULK_PAYLOAD);
    assert_int_equal(conn->rbuf_size, 0); // Consumato tutto

    // Chunk 2: "GET\r\n" (5 byte)
    memcpy(conn->rbuf, full_buf + 8, 5);
    conn->rbuf_size = 5;
    consume_buffer(conn);

    // Ha finito il primo argomento, aspetta il tipo del secondo ($)
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE);
    assert_string_equal(conn->argv[0], "GET");

    // Chunk 3: "$5\r\n" (4 byte)
    memcpy(conn->rbuf, full_buf + 13, 4);
    conn->rbuf_size = 4;
    consume_buffer(conn);

    // Aspetta payload
    assert_int_equal(conn->parse_state, RESP_PARSE_BULK_PAYLOAD);

    // Chunk 4: "a_key\r\n" (7 byte)
    memcpy(conn->rbuf, full_buf + 17, 7);
    conn->rbuf_size = 7;
    consume_buffer(conn);

    // Finito tutto
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE);
    assert_string_equal(conn->argv[1], "a_key");
}

// Test 3: Due comandi in un solo buffer (Pipelining)
static void test_parse_two_commands_at_once(void** state) {
    Conn* conn = (Conn*)*state;

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

    assert_string_equal(conn->argv[0], "PING");
    assert_int_equal(conn->rbuf_size, size2); // Deve rimanere PONG nel buffer
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE);

    // 2. Parsa il secondo comando (PONG)
    // Nota: consume_buffer rileva che siamo in RESP_PARSE_TYPE e c'è un argv vecchio,
    // quindi deve liberare "PING" prima di parsare "PONG".
    consume_buffer(conn);

    assert_string_equal(conn->argv[0], "PONG");
    assert_int_equal(conn->rbuf_size, 0); // Buffer vuoto
}

// Test 4: Comando array vuoto (es. Heartbeat custom o errore client)
static void test_parse_empty_command(void** state) {
    Conn* conn = (Conn*)*state;

    // *0\r\n
    char* empty_cmd = "*0\r\n";
    size_t len = strlen(empty_cmd);

    memcpy(conn->rbuf, empty_cmd, len);
    conn->rbuf_size = len;

    // Chiamiamo consume_buffer per parsare il comando
    consume_buffer(conn);

    // Eseguiamo il comando per generare la risposta
    execute_command(conn);

    // Verifichiamo che la risposta sia stata generata
    assert_int_equal(conn->parse_state, RESP_PARSE_TYPE); // Pronto per il prossimo comando
    assert_int_equal(conn->wbuf_size, 5); // Expected response "+OK\r\n" has length 5.
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
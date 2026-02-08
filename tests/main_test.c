#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h> // Per malloc/calloc/free
#include <cmocka.h>
#include <string.h>
#include <stdio.h>  // Per printf

#include "protocol.h"

// Helper per costruire un comando finto nel buffer secondo il nuovo protocollo
static void build_fake_command(uint8_t* buffer, size_t* size, int num_args, ...) {
    va_list args;
    va_start(args, num_args);

    uint32_t net_num_args = (uint32_t)num_args;
    memcpy(buffer, &net_num_args, 4);
    *size = 4;

    for (int i = 0; i < num_args; i++) {
        const char* arg = va_arg(args, const char*);
        uint32_t len = (uint32_t)strlen(arg);
        memcpy(buffer + *size, &len, 4);
        *size += 4;
        memcpy(buffer + *size, arg, len);
        *size += len;
    }

    va_end(args);
}

// Test 1: Un comando completo viene ricevuto e parsato in una sola volta.
static void test_parse_full_command_at_once(void** state) {
    (void)state;
    Conn* conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(conn);
    conn->parse_state = STATE_PARSE_INIT;

    build_fake_command(conn->rbuf, &conn->rbuf_size, 3, "SET", "mykey", "myvalue");

    consume_buffer(conn);

    assert_int_equal(conn->rbuf_size, 0);
    assert_int_equal(conn->parse_state, STATE_PARSE_NUM_ARGS); // Corrected expectation
    assert_int_not_equal(conn->wbuf_size, 0);

    free(conn);
}

// Test 2: Un comando viene ricevuto in più pezzi (chunks).
static void test_parse_command_in_chunks(void** state) {
    (void)state;
    Conn* conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(conn);
    conn->parse_state = STATE_PARSE_INIT;

    uint8_t full_command_buffer[K_MAX_MSG];
    size_t full_command_size;
    build_fake_command(full_command_buffer, &full_command_size, 2, "GET", "a_key");

    // 1. Simula l'arrivo dei primi 8 byte (num_args + len("GET"))
    memcpy(conn->rbuf, full_command_buffer, 8);
    conn->rbuf_size = 8;
    consume_buffer(conn);

    assert_int_equal(conn->parse_state, STATE_PARSE_ARG_PAYLOAD);
    assert_int_equal(conn->total_args_expected, 2);
    assert_int_equal(conn->current_arg_idx, 0);
    assert_int_equal(conn->arg_len, 3); // "GET"
    assert_int_equal(conn->rbuf_size, 0);

    // 2. Simula l'arrivo del resto
    memcpy(conn->rbuf, &full_command_buffer[8], full_command_size - 8);
    conn->rbuf_size = full_command_size - 8;
    consume_buffer(conn);

    assert_int_equal(conn->rbuf_size, 0);
    assert_int_equal(conn->parse_state, STATE_PARSE_NUM_ARGS); // Corrected expectation
    assert_int_not_equal(conn->wbuf_size, 0);

    free(conn);
}

// Test 3: Due comandi completi arrivano nello stesso buffer.
static void test_parse_two_commands_at_once(void** state) {
    (void)state;
    Conn* conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(conn);
    conn->parse_state = STATE_PARSE_INIT;

    uint8_t cmd1_buffer[K_MAX_MSG];
    size_t cmd1_size;
    build_fake_command(cmd1_buffer, &cmd1_size, 1, "PING");

    uint8_t cmd2_buffer[K_MAX_MSG];
    size_t cmd2_size;
    build_fake_command(cmd2_buffer, &cmd2_size, 1, "PONG");

    memcpy(conn->rbuf, cmd1_buffer, cmd1_size);
    memcpy(&conn->rbuf[cmd1_size], cmd2_buffer, cmd2_size);
    conn->rbuf_size = cmd1_size + cmd2_size;

    consume_buffer(conn);

    assert_int_equal(conn->rbuf_size, 0);
    assert_int_equal(conn->parse_state, STATE_PARSE_NUM_ARGS); // Corrected expectation

    uint32_t reply_len_ok = (uint32_t)strlen("OK");
    size_t expected_wbuf_size = (4 + reply_len_ok) * 2;
    assert_int_equal(conn->wbuf_size, expected_wbuf_size);

    free(conn);
}

// Test 4: Il parser gestisce un comando vuoto (0 argomenti).
static void test_parse_empty_command(void** state) {
    (void)state;
    Conn* conn = (Conn*)calloc(1, sizeof(Conn));
    assert_non_null(conn);
    conn->parse_state = STATE_PARSE_INIT;

    uint32_t num_args = 0;
    memcpy(conn->rbuf, &num_args, 4);
    conn->rbuf_size = 4;

    consume_buffer(conn);

    assert_int_equal(conn->rbuf_size, 0);
    assert_int_equal(conn->parse_state, STATE_PARSE_NUM_ARGS); // Corrected expectation
    assert_int_equal(conn->wbuf_size, 0);

    free(conn);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_full_command_at_once),
        cmocka_unit_test(test_parse_command_in_chunks),
        cmocka_unit_test(test_parse_two_commands_at_once),
        cmocka_unit_test(test_parse_empty_command),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

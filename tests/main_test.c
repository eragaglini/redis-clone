#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cmocka.h>

#include "protocol.h"
#include "store.h" 

// --- SETUP & TEARDOWN ---

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
    test_state->conn->queued_cmds_head = NULL;
    test_state->conn->queued_cmds_tail = NULL;
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

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
            // Usa le tue funzioni di pulizia aggiornate (definite nel server.c o protocol.c)
            // Se non sono visibili qui, dovresti includerle o mockarle. 
            // Per ora usiamo free_command manuale se la lista non è vuota.
            Command* c = test_state->conn->cmd_list_head;
            while(c) {
                Command* next = c->next;
                // Qui dovremmo usare command_free(c)
                // Assumiamo di usare l'helper locale free_command per i test
                // Nota: free_command qui sotto è adattata.
                if (c->args) {
                    for(uint32_t i=0; i<c->argc; i++) free(c->args[i].data);
                    free(c->args);
                }
                free(c);
                c = next;
            }
            
            // Stessa cosa per queued_cmds...
             Command* q = test_state->conn->queued_cmds_head;
            while(q) {
                Command* next = q->next;
                if (q->args) {
                    for(uint32_t i=0; i<q->argc; i++) free(q->args[i].data);
                    free(q->args);
                }
                free(q);
                q = next;
            }

            free(test_state->conn);
        }
        if (test_state->store) {
            store_free(test_state->store);
            free(test_state->store);
        }
        free(test_state);
    }
    return 0;
}

// --- HELPER AGGIORNATI (BINARY SAFE) ---

static Command* create_command(int argc, ...) {
    Command* cmd = (Command*)calloc(1, sizeof(Command));
    assert_non_null(cmd);
    cmd->argc = argc;
    // Allocazione array di struct Argument
    cmd->args = (Argument*)calloc(argc, sizeof(Argument));
    assert_non_null(cmd->args);

    va_list args;
    va_start(args, argc);
    for (int i = 0; i < argc; i++) {
        const char* arg_str = va_arg(args, const char*);
        size_t len = strlen(arg_str);
        
        // Allocazione dati binari
        cmd->args[i].data = malloc(len + 1); // +1 per sicurezza nei test (printf), ma usiamo len
        assert_non_null(cmd->args[i].data);
        memcpy(cmd->args[i].data, arg_str, len);
        cmd->args[i].data[len] = '\0'; // Null-terminate solo per comodità di debug
        
        cmd->args[i].len = len;
    }
    va_end(args);
    return cmd;
}

// Helper per accodare manualmente un comando creato con create_command
static void manual_append_command(Conn* conn, Command* cmd) {
    if (conn->cmd_list_tail) {
        conn->cmd_list_tail->next = cmd;
        conn->cmd_list_tail = cmd;
    } else {
        conn->cmd_list_head = cmd;
        conn->cmd_list_tail = cmd;
    }
    cmd->next = NULL;
}

// Build fake command rimane quasi uguale perché simula il byte stream del client
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

// --- UNIT TESTS ---

static void test_parse_full_command_at_once(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    uint8_t buf[K_MAX_MSG];
    size_t size;
    build_fake_command(buf, K_MAX_MSG, &size, 3, "SET", "key", "val");

    memcpy(conn->rbuf, buf, size);
    conn->rbuf_size = size;

    consume_buffer(conn);

    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;

    // VERIFICHE AGGIORNATE ALLA NUOVA STRUCT
    assert_int_equal(cmd->argc, 3);
    
    // Check arg 0
    assert_int_equal(cmd->args[0].len, 3);
    assert_memory_equal(cmd->args[0].data, "SET", 3);
    
    // Check arg 1
    assert_int_equal(cmd->args[1].len, 3);
    assert_memory_equal(cmd->args[1].data, "key", 3);

    // Check arg 2
    assert_int_equal(cmd->args[2].len, 3);
    assert_memory_equal(cmd->args[2].data, "val", 3);

    assert_null(cmd->next);

    execute_command(conn, test_state->store);

    assert_null(conn->cmd_list_head);
    assert_int_equal(conn->wbuf_size, 5);
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
}

static void test_parse_command_in_chunks(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    uint8_t full_buf[K_MAX_MSG];
    size_t full_size;
    build_fake_command(full_buf, K_MAX_MSG, &full_size, 2, "GET", "a_key");

    // Chunk 1
    memcpy(conn->rbuf, full_buf, 8);
    conn->rbuf_size = 8;
    consume_buffer(conn);
    assert_null(conn->cmd_list_head);

    // Chunk 2
    memcpy(conn->rbuf, full_buf + 8, 5);
    conn->rbuf_size = 5;
    consume_buffer(conn);
    assert_null(conn->cmd_list_head);

    // Chunk 3
    memcpy(conn->rbuf, full_buf + 13, 4);
    conn->rbuf_size = 4;
    consume_buffer(conn);
    assert_null(conn->cmd_list_head);

    // Chunk 4
    memcpy(conn->rbuf, full_buf + 17, 7);
    conn->rbuf_size = 7;
    consume_buffer(conn); 

    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;

    assert_int_equal(cmd->argc, 2);
    assert_memory_equal(cmd->args[0].data, "GET", 3);
    assert_memory_equal(cmd->args[1].data, "a_key", 5);

    execute_command(conn, test_state->store);
    assert_string_equal((char*)conn->wbuf, "$-1\r\n");
}

static void test_parse_two_commands_at_once(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    uint8_t buf1[K_MAX_MSG];
    size_t size1;
    build_fake_command(buf1, K_MAX_MSG, &size1, 1, "PING");

    uint8_t buf2[K_MAX_MSG];
    size_t size2;
    build_fake_command(buf2, K_MAX_MSG, &size2, 1, "PONG");

    memcpy(conn->rbuf, buf1, size1);
    memcpy(conn->rbuf + size1, buf2, size2);
    conn->rbuf_size = size1 + size2;

    consume_buffer(conn);

    assert_non_null(conn->cmd_list_head);
    assert_memory_equal(conn->cmd_list_head->args[0].data, "PING", 4);
    assert_null(conn->cmd_list_head->next);

    consume_buffer(conn);

    assert_non_null(conn->cmd_list_head->next);
    assert_memory_equal(conn->cmd_list_head->next->args[0].data, "PONG", 4);

    execute_command(conn, test_state->store);
    assert_string_equal((char*)conn->wbuf, "+PONG\r\n+OK\r\n");
}

static void test_parse_empty_command(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    char* empty_cmd_str = "*0\r\n";
    size_t len = strlen(empty_cmd_str);

    memcpy(conn->rbuf, (uint8_t*)empty_cmd_str, len);
    conn->rbuf_size = len;

    consume_buffer(conn);

    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;
    assert_int_equal(cmd->argc, 0);
    // Nota: args potrebbe essere NULL o allocato a 0 a seconda della malloc(0)
    // L'importante è che argc sia 0.
    
    execute_command(conn, test_state->store);
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
}

// --- TEST COMMAND REPLIES (Aggiornati) ---

static void test_get_command_reply_set_get_string(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;
    
    // Per testare get_command_reply, simuliamo che il comando sia nella connessione
    // o semplicemente passiamo il comando se la firma lo permette.
    // Assumiamo che execute_command legga dalla lista.

    // 1. SET
    cmd = create_command(3, "SET", "mykey", "myvalue");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, "+OK\r\n");
    
    // Reset buffer
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // 2. GET
    cmd = create_command(2, "GET", "mykey");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, "$7\r\nmyvalue\r\n");
    
    // Reset buffer
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // 3. GET non-existent
    cmd = create_command(2, "GET", "nonexistent");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, "$-1\r\n");
    
    // Reset
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));
}

static void test_get_command_reply_hset_hget_hlen(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;

    // HSET
    cmd = create_command(4, "HSET", "myhash", "field1", "value1");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, ":1\r\n");
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // HGET
    cmd = create_command(3, "HGET", "myhash", "field1");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, "$6\r\nvalue1\r\n");
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));
    
    // HLEN
    cmd = create_command(2, "HLEN", "myhash");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, ":1\r\n");
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));
}

static void test_execute_command_multi_exec(void** state) {
    TestState* test_state = (TestState*)*state;
    Command *cmd;

    // MULTI
    cmd = create_command(1, "MULTI");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_true(test_state->conn->in_transaction);
    assert_string_equal((char*)test_state->conn->wbuf, "+OK\r\n"); 
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // QUEUED SET
    cmd = create_command(3, "SET", "txkey", "txval");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_non_null(test_state->conn->queued_cmds_head);
    assert_string_equal((char*)test_state->conn->wbuf, "+QUEUED\r\n"); 
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // EXEC
    cmd = create_command(1, "EXEC");
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_false(test_state->conn->in_transaction);
    
    // Expected output: Array of results
    assert_true(strstr((char*)test_state->conn->wbuf, "*1\r\n"));
    assert_true(strstr((char*)test_state->conn->wbuf, "+OK\r\n"));
    test_state->conn->wbuf_size = 0; memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));
}

static Command* create_command_binary(int argc, ...) {
    Command* cmd = (Command*)calloc(1, sizeof(Command));
    assert_non_null(cmd);
    cmd->argc = argc;
    cmd->args = (Argument*)calloc(argc, sizeof(Argument));
    assert_non_null(cmd->args);

    va_list args;
    va_start(args, argc);
    for (int i = 0; i < argc; i++) {
        Argument* arg = va_arg(args, Argument*);
        cmd->args[i].data = malloc(arg->len);
        assert_non_null(cmd->args[i].data);
        memcpy(cmd->args[i].data, arg->data, arg->len);
        cmd->args[i].len = arg->len;
    }
    va_end(args);
    return cmd;
}

static void build_fake_command_binary(uint8_t* buffer, size_t buffer_size, size_t* size, int num_args, ...) {
    va_list args;
    va_start(args, num_args);
    *size = 0;
    int n = snprintf((char*)buffer + *size, buffer_size - *size, "*%d\r\n", num_args);
    *size += n;
    for (int i = 0; i < num_args; i++) {
        Argument* arg = va_arg(args, Argument*);
        n = snprintf((char*)buffer + *size, buffer_size - *size, "$%zu\r\n", arg->len);
        *size += n;
        memcpy(buffer + *size, arg->data, arg->len);
        *size += arg->len;
        n = snprintf((char*)buffer + *size, buffer_size - *size, "\r\n");
        *size += n;
    }
    va_end(args);
}

static void test_set_get_binary_data(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;

    char bin_val[] = "value\0with\0nulls";
    Argument key = { .data = "binkey", .len = 6 };
    Argument val = { .data = bin_val, .len = sizeof(bin_val) - 1 };

    // 1. SET
    cmd = create_command_binary(3, &((Argument){.data="SET", .len=3}), &key, &val);
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, "+OK\r\n");
    
    // Reset buffer
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // 2. GET
    cmd = create_command_binary(2, &((Argument){.data="GET", .len=3}), &key);
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);

    char expected_reply[100];
    int len = snprintf(expected_reply, sizeof(expected_reply), "$%zu\r\n", val.len);
    memcpy(expected_reply + len, val.data, val.len);
    len += val.len;
    memcpy(expected_reply + len, "\r\n", 2);
    len += 2;
    expected_reply[len] = '\0';

    assert_int_equal(test_state->conn->wbuf_size, len);
    assert_memory_equal(test_state->conn->wbuf, expected_reply, len);
}

static void test_hset_hget_binary_data(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;

    char bin_val[] = "value\0with\0nulls";
    Argument hkey = { .data = "h_binkey", .len = 8 };
    Argument hfield = { .data = "h_binfield", .len = 10 };
    Argument hval = { .data = bin_val, .len = sizeof(bin_val) - 1 };

    // 1. HSET
    cmd = create_command_binary(4, &((Argument){.data="HSET", .len=4}), &hkey, &hfield, &hval);
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);
    assert_string_equal((char*)test_state->conn->wbuf, ":1\r\n");

    // Reset buffer
    test_state->conn->wbuf_size = 0;
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf));

    // 2. HGET
    cmd = create_command_binary(3, &((Argument){.data="HGET", .len=4}), &hkey, &hfield);
    manual_append_command(test_state->conn, cmd);
    execute_command(test_state->conn, test_state->store);

    char expected_reply[100];
    int len = snprintf(expected_reply, sizeof(expected_reply), "$%zu\r\n", hval.len);
    memcpy(expected_reply + len, hval.data, hval.len);
    len += hval.len;
    memcpy(expected_reply + len, "\r\n", 2);
    len += 2;
    expected_reply[len] = '\0';

    assert_int_equal(test_state->conn->wbuf_size, len);
    assert_memory_equal(test_state->conn->wbuf, expected_reply, len);
}

static void test_parse_binary_safe_command(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;

    uint8_t buf[K_MAX_MSG];
    size_t size;
    char bin_val[] = "value\0with\0nulls";
    Argument key = { .data = "binkey", .len = 6 };
    Argument val = { .data = bin_val, .len = sizeof(bin_val) - 1 };
    build_fake_command_binary(buf, K_MAX_MSG, &size, 3, &((Argument){.data="SET", .len=3}), &key, &val);

    memcpy(conn->rbuf, buf, size);
    conn->rbuf_size = size;

    consume_buffer(conn);

    assert_non_null(conn->cmd_list_head);
    Command* cmd = conn->cmd_list_head;

    assert_int_equal(cmd->argc, 3);
    
    assert_int_equal(cmd->args[0].len, 3);
    assert_memory_equal(cmd->args[0].data, "SET", 3);
    
    assert_int_equal(cmd->args[1].len, 6);
    assert_memory_equal(cmd->args[1].data, "binkey", 6);

    assert_int_equal(cmd->args[2].len, val.len);
    assert_memory_equal(cmd->args[2].data, val.data, val.len);

    assert_null(cmd->next);

    execute_command(conn, test_state->store);

    assert_null(conn->cmd_list_head);
    assert_int_equal(conn->wbuf_size, 5);
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
}

// --- MAIN ---

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_parse_full_command_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_command_in_chunks, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_two_commands_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_empty_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_set_get_string, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_hset_hget_hlen, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_command_multi_exec, setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_get_binary_data, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hset_hget_binary_data, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_binary_safe_command, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
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
    test_state->conn->wbuf_size = 0; // Clear wbuf size
    memset(test_state->conn->wbuf, 0, sizeof(test_state->conn->wbuf)); // Clear wbuf content


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
            free_queued_command_list(test_state->conn); // Pulisce la lista di comandi in coda
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

static Command* create_command(int argc, ...) {
    Command* cmd = (Command*)calloc(1, sizeof(Command));
    assert_non_null(cmd);
    cmd->argc = argc;
    cmd->argv = (char**)calloc(argc, sizeof(char*));
    assert_non_null(cmd->argv);

    va_list args;
    va_start(args, argc);
    for (int i = 0; i < argc; i++) {
        const char* arg = va_arg(args, const char*);
        cmd->argv[i] = strdup(arg);
        assert_non_null(cmd->argv[i]);
    }
    va_end(args);
    return cmd;
}

static void free_command(Command* cmd) {
    if (cmd) {
        if (cmd->argv) {
            for (uint32_t i = 0; i < cmd->argc; i++) {
                if (cmd->argv[i]) {
                    free(cmd->argv[i]);
                }
            }
            free(cmd->argv);
        }
        free(cmd);
    }
}

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

// --- ORIGINAL PARSING UNIT TESTS ---

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

// --- get_command_reply UNIT TESTS ---

static void test_get_command_reply_ping(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd = create_command(1, "PING");
    char* reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "+PONG\r\n");
    free(reply);
    free_command(cmd);

    cmd = create_command(2, "PING", "hello");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "$5\r\nhello\r\n");
    free(reply);
    free_command(cmd);
}

static void test_get_command_reply_set_get_string(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;
    char* reply;

    // SET key value
    cmd = create_command(3, "SET", "mykey", "myvalue");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "+OK\r\n");
    free(reply);
    free_command(cmd);

    // GET key
    cmd = create_command(2, "GET", "mykey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "$7\r\nmyvalue\r\n");
    free(reply);
    free_command(cmd);

    // GET non-existent key
    cmd = create_command(2, "GET", "nonexistent");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "$-1\r\n");
    free(reply);
    free_command(cmd);
}

static void test_get_command_reply_hset_hget_hlen(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;
    char* reply;

    // HSET myhash field1 value1 (new field)
    cmd = create_command(4, "HSET", "myhash", "field1", "value1");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);

    // HSET myhash field2 value2 (new field)
    cmd = create_command(4, "HSET", "myhash", "field2", "value2");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);

    // HSET myhash field1 newvalue1 (updated field)
    cmd = create_command(4, "HSET", "myhash", "field1", "newvalue1");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);

    // HGET myhash field1
    cmd = create_command(3, "HGET", "myhash", "field1");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "$9\r\nnewvalue1\r\n");
    free(reply);
    free_command(cmd);

    // HGET myhash nonexistentfield
    cmd = create_command(3, "HGET", "myhash", "nonexistentfield");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "$-1\r\n");
    free(reply);
    free_command(cmd);

    // HLEN myhash
    cmd = create_command(2, "HLEN", "myhash");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":2\r\n");
    free(reply);
    free_command(cmd);

    // HLEN nonexistenthash
    cmd = create_command(2, "HLEN", "nonexistenthash");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);

    // Try HSET on a string key (should error)
    cmd = create_command(3, "SET", "stringkey", "stringval");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    free(reply);
    free_command(cmd); // Dispose SET command

    cmd = create_command(4, "HSET", "stringkey", "field", "value");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "-ERR HSET failed\r\n"); // store_hset returns -1
    free(reply);
    free_command(cmd);
}

static void test_get_command_reply_hdel_hgetall(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;
    char* reply;

    // Prep: HSET some fields
    cmd = create_command(4, "HSET", "myhashdelgetall", "f1", "v1");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);
    cmd = create_command(4, "HSET", "myhashdelgetall", "f2", "v2");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);
    cmd = create_command(4, "HSET", "myhashdelgetall", "f3", "v3");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);

    // HDEL existing field
    cmd = create_command(3, "HDEL", "myhashdelgetall", "f2");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);

    // HDEL non-existent field
    cmd = create_command(3, "HDEL", "myhashdelgetall", "nonexistent");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);

    // HGETALL
    cmd = create_command(2, "HGETALL", "myhashdelgetall");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    // Note: order is not guaranteed, but we can check for elements
    // "*4\r\n$2\r\nf1\r\n$2\r\nv1\r\n$2\r\nf3\r\n$2\r\nv3\r\n"
    assert_true(strstr(reply, "$2\r\nf1\r\n"));
    assert_true(strstr(reply, "$2\r\nv1\r\n"));
    assert_false(strstr(reply, "$2\r\nf2\r\n")); // f2 should be deleted
    assert_false(strstr(reply, "$2\r\nv2\r\n"));
    assert_true(strstr(reply, "$2\r\nf3\r\n"));
    assert_true(strstr(reply, "$2\r\nv3\r\n"));
    assert_true(strstr(reply, "*4\r\n")); // Should have 4 elements (2 fields + 2 values)
    free(reply);
    free_command(cmd);

    // HGETALL on non-existent hash
    cmd = create_command(2, "HGETALL", "nonexistenthash");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "*0\r\n"); // Redis returns empty array
    free(reply);
    free_command(cmd);

    // HGETALL on string key (should error WRONGTYPE)
    cmd = create_command(3, "SET", "stringforkey", "stringvalue");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);

    cmd = create_command(2, "HGETALL", "stringforkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
    free(reply);
    free_command(cmd);
}

static void test_get_command_reply_del_exists_type(void** state) {
    TestState* test_state = (TestState*)*state;
    Command* cmd;
    char* reply;

    // Prep: SET a string and HSET a hash
    cmd = create_command(3, "SET", "strkey", "strval");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);
    cmd = create_command(4, "HSET", "hashkey", "f1", "v1");
    reply = get_command_reply(test_state->conn, cmd, test_state->store); free(reply); free_command(cmd);

    // EXISTS strkey
    cmd = create_command(2, "EXISTS", "strkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);

    // EXISTS hashkey
    cmd = create_command(2, "EXISTS", "hashkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);

    // EXISTS nonexistent
    cmd = create_command(2, "EXISTS", "nonexistent");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);

    // TYPE strkey
    cmd = create_command(2, "TYPE", "strkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "+string\r\n");
    free(reply);
    free_command(cmd);

    // TYPE hashkey
    cmd = create_command(2, "TYPE", "hashkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "+hash\r\n");
    free(reply);
    free_command(cmd);

    // TYPE nonexistent
    cmd = create_command(2, "TYPE", "nonexistent");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, "+none\r\n");
    free(reply);
    free_command(cmd);

    // DEL single key (strkey)
    cmd = create_command(2, "DEL", "strkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n");
    free(reply);
    free_command(cmd);
    cmd = create_command(2, "EXISTS", "strkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);

    // DEL multiple keys (hashkey, nonexistent)
    cmd = create_command(3, "DEL", "hashkey", "nonexistent");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":1\r\n"); // Only hashkey should be deleted
    free(reply);
    free_command(cmd);
    cmd = create_command(2, "EXISTS", "hashkey");
    reply = get_command_reply(test_state->conn, cmd, test_state->store);
    assert_string_equal(reply, ":0\r\n");
    free(reply);
    free_command(cmd);
}


// --- execute_command UNIT TESTS ---

static void test_execute_command_set_get(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    HashMap* store = test_state->store;

    // SET command
    Command* cmd_set = create_command(3, "SET", "k1", "v1");
    cmd_create_and_append(conn, cmd_set->argv, cmd_set->argc, &conn->cmd_list_head, &conn->cmd_list_tail);
    cmd_set->argv = NULL; // Ownership transferred

    execute_command(conn, store);
    conn->wbuf[conn->wbuf_size] = '\0'; // Null-terminate for assert_string_equal
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
    conn->wbuf_size = 0; // Clear size for next test
    memset(conn->wbuf, 0, sizeof(conn->wbuf)); // Clear buffer content

    // GET command
    Command* cmd_get = create_command(2, "GET", "k1");
    cmd_create_and_append(conn, cmd_get->argv, cmd_get->argc, &conn->cmd_list_head, &conn->cmd_list_tail);
    cmd_get->argv = NULL; // Ownership transferred

    execute_command(conn, store);
    conn->wbuf[conn->wbuf_size] = '\0'; // Null-terminate for assert_string_equal
    assert_string_equal((char*)conn->wbuf, "$2\r\nv1\r\n");
    conn->wbuf_size = 0;
    memset(conn->wbuf, 0, sizeof(conn->wbuf));

    // GET non-existent
    Command* cmd_get_non = create_command(2, "GET", "nonexistent");
    cmd_create_and_append(conn, cmd_get_non->argv, cmd_get_non->argc, &conn->cmd_list_head, &conn->cmd_list_tail);
    cmd_get_non->argv = NULL; // Ownership transferred

    execute_command(conn, store);
    conn->wbuf[conn->wbuf_size] = '\0'; // Null-terminate for assert_string_equal
    assert_string_equal((char*)conn->wbuf, "$-1\r\n");
    conn->wbuf_size = 0;
    memset(conn->wbuf, 0, sizeof(conn->wbuf));
}

static void test_execute_command_multi_exec(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    HashMap* store = test_state->store;
    Command *cmd;
    char *reply;

    // MULTI
    cmd = create_command(1, "MULTI");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    assert_true(conn->in_transaction);
    assert_string_equal((char*)conn->wbuf, "+OK\r\n"); conn->wbuf_size = 0;

    // Queued SET
    cmd = create_command(3, "SET", "txkey", "txval");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    assert_non_null(conn->queued_cmds_head);
    assert_string_equal((char*)conn->wbuf, "+QUEUED\r\n"); conn->wbuf_size = 0;

    // Queued HSET
    cmd = create_command(4, "HSET", "txhash", "f1", "v1");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    assert_non_null(conn->queued_cmds_head->next);
    assert_string_equal((char*)conn->wbuf, "+QUEUED\r\n"); conn->wbuf_size = 0;

    // EXEC
    cmd = create_command(1, "EXEC");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    assert_false(conn->in_transaction);
    assert_null(conn->queued_cmds_head); // Queued commands should be cleared

    // Expected EXEC reply: array of +OK\r\n and :1\r\n
    // "*2\r\n+OK\r\n:1\r\n"
    assert_true(strstr((char*)conn->wbuf, "*2\r\n"));
    assert_true(strstr((char*)conn->wbuf, "+OK\r\n"));
    assert_true(strstr((char*)conn->wbuf, ":1\r\n"));
    conn->wbuf_size = 0;

    // Verify commands were executed
    cmd = create_command(2, "GET", "txkey");
    reply = get_command_reply(conn, cmd, store);
    assert_string_equal(reply, "$5\r\ntxval\r\n"); free(reply); free_command(cmd);

    cmd = create_command(3, "HGET", "txhash", "f1");
    reply = get_command_reply(conn, cmd, store);
    assert_string_equal(reply, "$2\r\nv1\r\n"); free(reply); free_command(cmd);
}

static void test_execute_command_multi_discard(void** state) {
    TestState* test_state = (TestState*)*state;
    Conn* conn = test_state->conn;
    HashMap* store = test_state->store;
    Command *cmd;

    // MULTI
    cmd = create_command(1, "MULTI");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    conn->wbuf_size = 0;

    // Queued SET
    cmd = create_command(3, "SET", "txkeydiscard", "txvaldiscard");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    conn->wbuf_size = 0;

    // DISCARD
    cmd = create_command(1, "DISCARD");
    cmd_create_and_append(conn, cmd->argv, cmd->argc, &conn->cmd_list_head, &conn->cmd_list_tail); cmd->argv = NULL;
    execute_command(conn, store);
    assert_false(conn->in_transaction);
    assert_null(conn->queued_cmds_head); // Queued commands should be cleared
    conn->wbuf[conn->wbuf_size] = '\0'; // Null-terminate for assert_string_equal
    assert_string_equal((char*)conn->wbuf, "+OK\r\n");
    conn->wbuf_size = 0;
    memset(conn->wbuf, 0, sizeof(conn->wbuf));

    // Verify commands were NOT executed
    cmd = create_command(2, "GET", "txkeydiscard");
    char* reply = get_command_reply(conn, cmd, store);
    assert_string_equal(reply, "$-1\r\n"); free(reply); free_command(cmd);
}

// --- MAIN ---

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_parse_full_command_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_command_in_chunks, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_two_commands_at_once, setup, teardown),
        cmocka_unit_test_setup_teardown(test_parse_empty_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_ping, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_set_get_string, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_hset_hget_hlen, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_hdel_hgetall, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_command_reply_del_exists_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_command_set_get, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_command_multi_exec, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_command_multi_discard, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
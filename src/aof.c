#include "aof.h"
#include "protocol.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

// Global flag to prevent re-logging commands during AOF loading.
static int g_aof_loading = 0;
static const char* AOF_FILE_NAME = "appendonly.aof";

// Helper to compare bulk string with a C-string, case-insensitively.
static int argument_strcasecmp(Argument arg, const char* c_str) {
    if (arg.len != strlen(c_str)) {
        return -1;
    }
    return strncasecmp(arg.data, c_str, arg.len);
}

void aof_log(size_t num_args, Argument* args) {
    if (g_aof_loading || num_args == 0) {
        return;
    }

    // Filter for modification commands
    if (argument_strcasecmp(args[0], "SET") != 0 &&
        argument_strcasecmp(args[0], "DEL") != 0 &&
        argument_strcasecmp(args[0], "HSET") != 0 &&
        argument_strcasecmp(args[0], "HDEL") != 0 &&
        argument_strcasecmp(args[0], "FLUSHDB") != 0) {
        return;
    }

    FILE* fp = fopen(AOF_FILE_NAME, "ab");
    if (!fp) {
        perror("fopen aof");
        return;
    }

    // Write array header
    fprintf(fp, "*%zu\r\n", num_args);

    // Write each argument
    for (size_t i = 0; i < num_args; i++) {
        fprintf(fp, "$%zu\r\n", args[i].len);
        fwrite(args[i].data, 1, args[i].len, fp);
        fwrite("\r\n", 1, 2, fp);
    }

    fflush(fp); // Ensure data is written to disk
    fclose(fp);
}

// Correctly initializes a Conn struct for AOF loading
static Conn* create_dummy_conn() {
    Conn* c = (Conn*)malloc(sizeof(Conn));
    if (!c) {
        return NULL;
    }
    // Initialize Conn fields
    c->fd = -1; // No real file descriptor
    c->rbuf_size = 0; 
    c->wbuf_size = 0; 
    c->wbuf_sent = 0;
    c->parse_state = RESP_PARSE_TYPE;
    c->argc = 0;
    c->current_arg_idx = 0;
    c->resp_expected_len = 0;
    c->args = NULL;
    c->error = 0;
    c->last_activity_time = 0;
    c->cmd_list_head = NULL;
    c->cmd_list_tail = NULL;
    c->in_transaction = false;
    c->queued_cmds_head = NULL;
    c->queued_cmds_tail = NULL;
    return c;
}

// Correctly frees the dummy Conn struct
static void free_dummy_conn(Conn* c) {
    if (!c) {
        return;
    }
    // These lists should be empty after consume_buffer and execute_command, but free them just in case.
    free_command_list(c);
    free_queued_command_list(c);
    free_args(c); // free_args handles the c->args array
    free(c);
}

int aof_load(HashMap* store) {
    g_aof_loading = 1;

    FILE* fp = fopen(AOF_FILE_NAME, "rb");
    if (!fp) {
        // This is not an error if the file doesn't exist.
        g_aof_loading = 0;
        return 0;
    }

    Conn* conn = create_dummy_conn();
    if (!conn) {
        fprintf(stderr, "Failed to create dummy connection for AOF loading\n");
        fclose(fp);
        g_aof_loading = 0;
        return -1;
    }
    
    // Read the AOF file in chunks and process them
    while(1) {
        size_t space_left = sizeof(conn->rbuf) - conn->rbuf_size;
        size_t bytes_read = fread(conn->rbuf + conn->rbuf_size, 1, space_left, fp);
        if (bytes_read == 0) {
            if (ferror(fp)) {
                perror("fread from aof");
            }
            // We either hit EOF or an error. Stop reading.
            break;
        }

        conn->rbuf_size += bytes_read;

        // Consume as many commands as possible from the buffer
        while (conn->rbuf_size > 0) {
            size_t old_rbuf_size = conn->rbuf_size;
            consume_buffer(conn);
            if (conn->rbuf_size == old_rbuf_size) {
                break;
            }
        }
    }

    if (conn->cmd_list_head) {
        execute_command(conn, store, 1);
    }

    // After loading, check if there's any unprocessed data left.
    if (conn->rbuf_size > 0) {
        fprintf(stderr, "Warning: AOF file may have ended with an incomplete command.\n");
    }

    printf("AOF loaded.\n");

    free_dummy_conn(conn);
    fclose(fp);
    g_aof_loading = 0;
    return 0;
}

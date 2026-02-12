# Redis Clone Project Overview

This document provides a high-level overview of the "Redis Clone" project, which is a rudimentary implementation of a Redis-like key-value store. The project is primarily written in C and features a non-blocking, event-driven server architecture.

## Project Structure

-   **`.gitignore`**: Standard Git ignore file.
-   **`Makefile`**: Manages the build process, including compilation with `gcc` and linking of test components, and supports debug mode. It also includes targets for running unit and integration tests.
-   **`README.md`**: Project README.
-   **`bin/`**: Contains compiled executables (`main` for the server, `run_tests` for the C unit test suite).
-   **`include/`**: Empty, intended for public headers.
-   **`lib/cmocka/`**: Submodule containing the `cmocka` unit testing framework.
-   **`src/`**: Contains the core C source files for the server.
-   **`tests/`**: Contains unit tests for the server components (`tests/main_test.c`) and Python-based integration tests (`tests/test_integration.py`).


## Core Components

### `src/main.c`
The entry point of the server application. It is a simple wrapper that calls `server_run()` to start the server.

### `src/server.h` & `src/server.c`
These files define and implement the server's core functionality:
-   **Network I/O**: Handles socket creation, binding, listening, and accepting client connections.
-   **Event Loop**: Utilizes `poll()` for efficient I/O multiplexing, making the server non-blocking and event-driven. This allows it to handle multiple client connections concurrently without using threads per client.
-   **Connection Management**: Maintains a global `fd2conn` array to map file descriptors to `Conn` (connection) structures and a `poll_args` array for `poll()` monitoring.
-   **`handle_new_connection()`**: Accepts new clients, sets their sockets to non-blocking mode, and initializes a `Conn` structure to manage their state.
-   **`handle_client_io()`**: Reads incoming data from client sockets, delegates command parsing to `consume_buffer()`, and writes responses back to clients. It also handles client disconnections and errors.
-   **Non-blocking I/O**: Employs `fd_set_nb()` to configure sockets for non-blocking operations.
-   **DOS Mitigations**: Includes mechanisms for preventing Denial of Service through protocol limit validation and the implementation of timeouts for inactive connections.

### `src/protocol.h` & `src/protocol.c`
These files define the communication protocol and its parsing logic:
-   **`Conn` Struct**: Defined in `protocol.h`, this structure holds the state for each client connection, including read/write buffers (`rbuf`, `wbuf`), the current state of the command parser, and transaction state (`in_transaction`, `queued_cmds_head`, `queued_cmds_tail`).
-   **`ParseState` Enum**: Defines the states for the command parsing state machine (`STATE_PARSE_INIT`, `STATE_PARSE_NUM_ARGS`, `STATE_PARSE_ARG_LEN`, `STATE_PARSE_ARG_PAYLOAD`).
-   **`consume_buffer()`**: Implemented in `protocol.c`, this function is a state machine that parses incoming raw byte streams from the `Conn`'s read buffer.
    -   It now interprets a simplified version of the **Redis Serialization Protocol (RESP)** and correctly handles pipelined commands.
    -   Upon successful parsing, it delegates command execution to `execute_command()`. The commands `PING`, `SET`, `GET`, `HSET`, `HGET`, `HLEN`, `HDEL`, `HGETALL`, `DEL`, `EXISTS`, `TYPE`, `MULTI`, `EXEC`, and `DISCARD` are supported. Response generation now provides specific integer replies for HSET/HLEN/DEL/EXISTS and nil bulk strings for HGET when appropriate, aligning with Redis's RESP. TYPE command returns string replies like "+string", "+hash", "+none".

### `src/store.h` & `src/store.c`
These files define and implement the in-memory key-value store:
-   **`store.h`**: Declares the data structures (`Entry` now supports `ObjType` enum and `void* value`) and functions for the key-value store, including `store_init`, `store_set`, `store_get`, `store_delete_entry_from_map` (renamed from `store_del` for internal use), `store_free`, and new functions for Hash types: `store_hset`, `store_hget`, `store_hlen`, `store_hdel`, `store_hgetall`. Also includes new top-level key commands: `store_del` (for multiple keys), `store_exists`, and `store_type`.
-   **`store.c`**: Implements the hash map used for storing keys and values. It provides functions for setting and getting values, and now distinguishes between `OBJ_STRING` and `OBJ_HASH` types. `store_set` and `store_get` specifically handle string values. `store_hset`, `store_hget`, `store_hlen`, `store_hdel`, `store_hgetall` manage nested hash maps for `OBJ_HASH` types. `store_delete_entry_from_map` is the internal function for removing an entry from any `HashMap`. The new top-level `store_del` command can remove multiple keys. `store_exists` checks for key presence, and `store_type` returns the stored object type. `store_free` has been updated to recursively free memory based on the stored `ObjType`. Return values for functions have been refined to align with Redis's specific responses.



## Testing (`cmocka` unit tests, `pytest` integration tests)

-   **Frameworks**: The project uses `cmocka` for C unit testing and `pytest` for Python-based integration testing.
-   **`tests/main_test.c` (Unit Tests)**:
    -   **Test Focus**: Primarily targets the `consume_buffer` function in `protocol.c`.
    -   **Test Cases**: Cover scenarios such as:
        -   Parsing a complete command received at once.
        -   Parsing a command that arrives in fragmented chunks.
        -   Parsing multiple commands sent simultaneously (fully functional pipelining).
        -   Handling empty commands (0 arguments).
    -   **Build Integration**: The `Makefile` compiles `tests/main_test.c` and `protocol.c` (along with `cmocka` sources) into the `bin/run_tests` executable, run via `make test`.
-   **`tests/test_integration.py` (Integration Tests)**:
    -   **Test Focus**: Verifies the end-to-end functionality of the C server by interacting with it via a `redis-py` client.
    -   **Test Automation**: Uses `subprocess` to automatically start and stop the C server (`./bin/main`) for each test run.
    -   **Test Cases**: Includes tests for basic commands (`PING`, `SET`, `GET`), non-existent keys, simple command pipelining, hash commands (`HSET`, `HGET`, `HLEN`, `HDEL`, `HGETALL`), including scenarios for non-existent hash keys/fields and type mismatches. New tests cover generic key commands `DEL`, `EXISTS`, and `TYPE` for various key types and existence.
    -   **Execution**: Run via `make integration_test`.

## Current Limitations / Future Work

-   **Set of Commands Limited**: Currently, `PING`, `SET`, `GET`, `HSET`, `HGET`, `HLEN`, `MULTI`, `EXEC`, and `DISCARD` commands are implemented. Other standard Redis commands (e.g., `DEL`, `EXISTS`) need to be added to `execute_command`.
-   **Error Handling**: While more robust, error responses to clients are still somewhat generic ("-ERR ..."). More detailed and specific error messages would be beneficial.
-   **Scalability and Robustness**: For production use, further optimizations for scalability (e.g., thread pools, `epoll`/`kqueue` instead of `poll()`) and more granular error management are needed.
-   **Lack of Persistence**: The server does not save data to disk, meaning all data is lost upon server restart.
-   The `fd2conn` array's direct indexing by file descriptor has scalability limitations for very high numbers of connections or large file descriptor values, as noted in `server.c`. A more robust mapping (e.g., hash map) would be required for a production-grade server.

This overview should provide a solid foundation for understanding and further developing the Redis Clone project.
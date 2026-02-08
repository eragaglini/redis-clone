# Redis Clone Project Overview

This document provides a high-level overview of the "Redis Clone" project, which is a rudimentary and incomplete implementation of a Redis-like key-value store. The project is primarily written in C and features a non-blocking, event-driven server architecture.

## Project Structure

-   **`.gitignore`**: Standard Git ignore file.
-   **`client.py`**: A Python client demonstrating interaction with the C server using the defined custom protocol.
-   **`Makefile`**: Manages the build process, including compilation with `gcc` and linking of test components.
-   **`README.md`**: Project README.
-   **`bin/`**: Contains compiled executables (`main` for the server, `run_tests` for the test suite).
-   **`include/`**: Empty, intended for public headers.
-   **`lib/cmocka/`**: Submodule containing the `cmocka` unit testing framework.
-   **`src/`**: Contains the core C source files for the server.
-   **`tests/`**: Contains unit tests for the server components.

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

### `src/protocol.h` & `src/protocol.c`
These files define the communication protocol and its parsing logic:
-   **`Conn` Struct**: Defined in `protocol.h`, this structure holds the state for each client connection, including read/write buffers (`rbuf`, `wbuf`), and the current state of the command parser.
-   **`ParseState` Enum**: Defines the states for the command parsing state machine (`STATE_PARSE_INIT`, `STATE_PARSE_NUM_ARGS`, `STATE_PARSE_ARG_LEN`, `STATE_PARSE_ARG_PAYLOAD`).
-   **`consume_buffer()`**: Implemented in `protocol.c`, this function is a state machine that parses incoming raw byte streams from the `Conn`'s read buffer.
    -   It expects a custom binary protocol: `[4-byte num_args (little-endian)][4-byte arg1_len (little-endian)][arg1_payload][4-byte arg2_len][arg2_payload]...`
    -   Currently, upon successful parsing of a command, it sends a hardcoded "OK" response to the client. The actual command logic (e.g., SET, GET) is not yet implemented.

## Client Interaction (`client.py`)

-   A Python script that demonstrates how to establish a connection with the C server.
-   It adheres to the custom binary protocol for sending commands and receiving responses.
-   Includes functionality to test both single command requests and command pipelining (sending multiple commands without waiting for intermediate responses).

## Testing (`tests/main_test.c`, `bin/run_tests`)

-   **Framework**: The project uses `cmocka` for unit testing.
-   **Test Focus**: Tests primarily target the `consume_buffer` function in `protocol.c`.
-   **Test Cases**: Cover scenarios such as:
    -   Parsing a complete command received at once.
    -   Parsing a command that arrives in fragmented chunks.
    -   Parsing multiple commands sent simultaneously (pipelining).
    -   Handling empty commands (0 arguments).
-   **Build Integration**: The `Makefile` compiles `tests/main_test.c` and `protocol.c` (along with `cmocka` sources) into the `bin/run_tests` executable.

## Current Limitations / Future Work

-   The server currently only parses commands and sends a generic "OK" response. The actual key-value store logic (e.g., storing data for `SET`, retrieving data for `GET`) is not yet implemented.
-   Error handling for malformed commands in `protocol.c` is basic; more robust error responses are needed.
-   The `fd2conn` array's direct indexing by file descriptor has scalability limitations for very high numbers of connections or large file descriptor values, as noted in `server.c`. A more robust mapping (e.g., hash map) would be required for a production-grade server.

This overview should provide a solid foundation for understanding and further developing the Redis Clone project.

# Compiler
CC = gcc

# Compiler flags
# To enable debug logs for main app, run: make DEBUG=1
DEBUG ?= 0
# Base CFLAGS per tutte le compilazioni
CFLAGS_BASE = -Wall -Isrc -Ilib/cmocka/include

# CFLAGS per build standard (main app)
CFLAGS = $(CFLAGS_BASE)
ifeq ($(DEBUG), 1)
	CFLAGS += -DDEBUG -g
endif

# CFLAGS per test compilation (sempre debug)
TEST_CFLAGS = $(CFLAGS_BASE) -DDEBUG -g

# Main application target
TARGET = bin/main
# Trova tutti i file .c nella directory src
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

# Test target
TEST_TARGET = bin/run_tests
# I file oggetto necessari per l'eseguibile di test
TEST_SRC_FILES = tests/main_test.c
TEST_OBJS = $(TEST_SRC_FILES:.c=.o) tests_protocol.o src/store.o src/aof.o # src/store.o è ora parte della lista degli oggetti di test

# Oggetti di CMocka
CMOCKA_SRC = lib/cmocka/src/cmocka.c
CMOCKA_OBJ = $(CMOCKA_SRC:.c=.o)


.PHONY: all clean run test integration_test

all: $(TARGET)

# --- Main Application Build ---
$(TARGET): $(OBJS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Regole esplicite per la compilazione dei file .c in .o
src/main.o: src/main.c
	$(CC) $(CFLAGS) -c $< -o $@

src/server.o: src/server.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/main_test.o: tests/main_test.c
	$(CC) $(TEST_CFLAGS) -c $< -o $@

src/protocol.o: src/protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

# Regola specifica per protocol.o quando incluso nella compilazione dei test
tests_protocol.o: src/protocol.c
	$(CC) $(TEST_CFLAGS) -c $< -o $@

src/store.o: src/store.c
	$(CC) $(TEST_CFLAGS) -c $< -o $@

src/aof.o: src/aof.c
	$(CC) $(TEST_CFLAGS) -c $< -o $@

lib/cmocka/src/cmocka.o: lib/cmocka/src/cmocka.c
	$(CC) $(TEST_CFLAGS) -c $< -o $@


# --- Test Build ---
test: $(TEST_TARGET)
	./$(TEST_TARGET)

# L'eseguibile di test dipende dagli oggetti di test e da cmocka
$(TEST_TARGET): $(TEST_OBJS) $(CMOCKA_OBJ)
	@mkdir -p bin
	# Usa TEST_CFLAGS per il linking dei test
	$(CC) $(TEST_CFLAGS) -o $(TEST_TARGET) $(TEST_OBJS) $(CMOCKA_OBJ)

# --- Documentation ---
doc:
	@echo "Generating Doxygen documentation..."
	doxygen Doxyfile
	@echo "Opening documentation..."
	open html/index.html

# --- Commands ---
run: all
	./$(TARGET)

integration_test: all
	@echo "Running integration tests..."
	python3 -m pytest tests/test_integration.py

clean:
	rm -f src/*.o tests/*.o lib/cmocka/src/*.o $(TARGET) $(TEST_TARGET)
	rm -rf html latex
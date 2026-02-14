#ifndef AOF_H
#define AOF_H

#include "protocol.h"

/**
 * @brief Logs a command to the Append-Only File (AOF).
 *
 * This function serializes a command back into the RESP protocol format and
 * appends it to the AOF file. Only commands that modify the data store
 * (e.g., SET, HSET, DEL) are logged.
 *
 * @param num_args The number of arguments in the command.
 * @param args An array of BulkString arguments representing the command.
 */
void aof_log(size_t num_args, Argument* args);

/**
 * @brief Loads data from the Append-Only File (AOF) into the data store.
 *
 * This function is called on server startup. It reads the AOF file,
 * parses the RESP commands, and executes them to restore the server's state.
 */
int aof_load(struct HashMap* store);

#endif // AOF_H

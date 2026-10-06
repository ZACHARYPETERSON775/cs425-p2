#ifndef RDT_IO_H
#define RDT_IO_H

#include "rdt_protocol.h"

struct rdt_io_context;

/*
 * Concrete operating-system adapter: owns the UDP socket, relay HELLO,
 * monotonic clock, and source/destination FILE.
 */

/**
 * Resolve/connect the relay, complete HELLO, and open the source/destination.
 * The sender's file size is returned through file_size when that pointer is
 * non-NULL. The receiver output is opened only after relay registration.
 *
 * @return An owned I/O context, or NULL on failure.
 */
struct rdt_io_context *rdt_io_open(const char *host, unsigned short port,
                                   const char *path,
                                   const struct rdt_options *options,
                                   int sender, uint64_t *file_size);

/**
 * Populate protocol callbacks backed by the supplied adapter context.
 * A NULL context clears the callback table; a NULL output pointer is ignored.
 */
void rdt_io_get_protocol_io(struct rdt_io_context *context,
                            struct rdt_protocol_io *io);

/** Close the file and UDP socket, release context; return -1 on close error. */
int rdt_io_close(struct rdt_io_context *context);

#endif

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

/** Drive the pure sender state machine using the concrete I/O adapter. */
int rdt_io_send(struct rdt_io_context *context, uint64_t file_size,
                const struct rdt_options *options);

/** Drive the pure receiver state machine using the concrete I/O adapter. */
int rdt_io_receive(struct rdt_io_context *context);

/** Close the file and UDP socket, release context; return -1 on close error. */
int rdt_io_close(struct rdt_io_context *context);

#ifdef TEST
int rdt_io_test_dispatch(enum rdt_protocol_action_type type, int receiver,
                         int status, int empty_actions, int finishing,
                         int invalid_sender);
int rdt_io_test_drive(int receiver, int null_options);
int rdt_io_test_transfer_status(int result, int close_result);
#endif

#endif

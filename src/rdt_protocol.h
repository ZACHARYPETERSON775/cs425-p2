#ifndef RDT_PROTOCOL_H
#define RDT_PROTOCOL_H

#include "rdt.h"

/**
 * Injectable operations used by the protocol state machines.
 *
 * The protocol layer owns RDT packet sequencing, acknowledgements, timeout
 * decisions, and retransmission. The adapter owns the implementations of
 * these network, clock, and file operations.
 */
struct rdt_protocol_io
{
  void *context; /**< Opaque adapter state passed to every callback. */
  /** Send exactly one datagram; return 0 on success and -1 on failure. */
  int (*send_datagram)(void *context, const uint8_t *packet, size_t length);
  /**
   * Receive a datagram. Return 0 and set length when received, 1 on timeout,
   * or -1 on I/O failure.
   */
  int (*receive_datagram)(void *context, uint8_t *packet, size_t capacity,
                          size_t *length, int timeout_ms);
  /** Return the current monotonic time in milliseconds. */
  uint64_t (*now_ms)(void *context);
  /** Read/write the complete requested byte count; return 0 or -1. */
  int (*read_file)(void *context, uint8_t *bytes, size_t length);
  int (*write_file)(void *context, const uint8_t *bytes, size_t length);
  /** Flush received file bytes to the destination; return 0 or -1. */
  int (*flush_file)(void *context);
};

/** Run the sender state machine over supplied datagram, clock, and file I/O. */
int rdt_protocol_send(const struct rdt_protocol_io *io,
                      uint64_t file_size,
                      const struct rdt_options *options);

/** Run the receiver state machine over supplied datagram, clock, and file I/O. */
int rdt_protocol_receive(const struct rdt_protocol_io *io,
                         const struct rdt_options *options);

#endif

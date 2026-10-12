#ifndef RDT_PROTOCOL_H
#define RDT_PROTOCOL_H

#include "rdt.h"

enum rdt_protocol_action_type
{
  RDT_ACTION_NONE,
  RDT_ACTION_READ_FILE,
  RDT_ACTION_WRITE_FILE,
  RDT_ACTION_FLUSH_FILE,
  RDT_ACTION_CLOSE_FILE,
  RDT_ACTION_SEND_PACKET,
  RDT_ACTION_WAIT_UNTIL,
  RDT_ACTION_COMPLETE,
  RDT_ACTION_FAILED
};

/** Actions contain encoded datagrams, file data, deadlines, or final status. */
struct rdt_protocol_action
{
  enum rdt_protocol_action_type type;
  uint64_t deadline_ms;
  size_t length;
  uint8_t bytes[RDT_MAX_PACKET];
  int status;
};

struct rdt_protocol_actions
{
  struct rdt_protocol_action items[66];
  size_t count;
};

struct rdt_protocol_packet
{
  enum rdt_packet_type type;
  uint32_t sequence;
  uint32_t payload_length;
  uint8_t payload[RDT_MAX_PAYLOAD];
};

/** Input events contain only protocol data and caller-supplied monotonic time. */
enum rdt_sender_event_type
{
  RDT_SENDER_EVENT_DATA_READY,
  RDT_SENDER_EVENT_ACK,
  RDT_SENDER_EVENT_TIMER,
  RDT_SENDER_EVENT_IO_FAILURE
};

struct rdt_sender_event
{
  enum rdt_sender_event_type type;
  uint64_t now_ms;
  const uint8_t *bytes;
  size_t length;
  struct rdt_protocol_packet packet;
};

struct rdt_sender_slot
{
  uint8_t bytes[RDT_MAX_PACKET];
  size_t length;
};

struct rdt_sender
{
  uint64_t file_size;
  uint64_t total_packets;
  uint64_t base;
  uint64_t next_sequence;
  uint64_t deadline_ms;
  unsigned int window_size;
  unsigned int timeout_ms;
  unsigned int consecutive_timeouts;
  int timer_active;
  int read_pending;
  int finishing;
  int done;
  struct rdt_sender_slot slots[64];
};

enum rdt_receiver_event_type
{
  RDT_RECEIVER_EVENT_PACKET,
  RDT_RECEIVER_EVENT_TIMER,
  RDT_RECEIVER_EVENT_FILE_OK,
  RDT_RECEIVER_EVENT_IO_FAILURE
};

struct rdt_receiver_event
{
  enum rdt_receiver_event_type type;
  uint64_t now_ms;
  struct rdt_protocol_packet packet;
};

enum rdt_receiver_phase
{
  RDT_RECEIVER_ACTIVE,
  RDT_RECEIVER_WRITING,
  RDT_RECEIVER_FLUSHING,
  RDT_RECEIVER_CLOSING,
  RDT_RECEIVER_LINGER,
  RDT_RECEIVER_DONE,
  RDT_RECEIVER_FAILED
};

struct rdt_receiver
{
  uint64_t expected;
  uint64_t idle_deadline_ms;
  uint64_t linger_deadline_ms;
  uint32_t fin_sequence;
  enum rdt_receiver_phase phase;
  uint8_t pending_payload[RDT_MAX_PAYLOAD];
  uint32_t pending_length;
};

/**
 * Initialize the pure sender state machine and return its first action.
 * The machine performs no I/O; callers satisfy requested actions and feed
 * their outcomes back through rdt_sender_step.
 */
int rdt_sender_init(struct rdt_sender *sender, uint64_t file_size,
                    unsigned int window_size, unsigned int timeout_ms,
                    uint64_t now_ms, struct rdt_protocol_actions *actions);

/** Feed one event into the sender and receive the resulting actions. */
int rdt_sender_step(struct rdt_sender *sender,
                    const struct rdt_sender_event *event,
                    struct rdt_protocol_actions *actions);

/** Initialize the pure receiver state machine and its idle deadline. */
int rdt_receiver_init(struct rdt_receiver *receiver, uint64_t now_ms,
                      struct rdt_protocol_actions *actions);

/** Feed one event into the receiver and receive the resulting actions. */
int rdt_receiver_step(struct rdt_receiver *receiver,
                      const struct rdt_receiver_event *event,
                      struct rdt_protocol_actions *actions);

#endif

#include "rdt_protocol.h"

#include <string.h>

#define DATA_MAX_TIMEOUTS 10U
#define FIN_MAX_TIMEOUTS 10U
#define FIN_LINGER_MS UINT64_C(2000)
#define RECEIVE_IDLE_TIMEOUT_MS UINT64_C(30000)

static void clear_actions(struct rdt_protocol_actions *actions)
{
  actions->count = 0U;
}

static struct rdt_protocol_action *add_action(
    struct rdt_protocol_actions *actions,
    enum rdt_protocol_action_type type)
{
  struct rdt_protocol_action *action;

  action = &actions->items[actions->count++];
  memset(action, 0, sizeof(*action));
  action->type = type;
  return action;
}

static void add_wait(struct rdt_protocol_actions *actions, uint64_t deadline)
{
  struct rdt_protocol_action *action =
      add_action(actions, RDT_ACTION_WAIT_UNTIL);
  action->deadline_ms = deadline;
}

static void add_packet(struct rdt_protocol_actions *actions,
                       enum rdt_packet_type type, uint32_t sequence,
                       const uint8_t *payload, uint32_t payload_length)
{
  struct rdt_protocol_action *action =
      add_action(actions, RDT_ACTION_SEND_PACKET);

  action->length = rdt_encode_packet(action->bytes, sizeof(action->bytes),
                                     type, sequence, payload, payload_length);
}

static void add_status_action(struct rdt_protocol_actions *actions,
                              enum rdt_protocol_action_type type, int status)
{
  struct rdt_protocol_action *action = add_action(actions, type);
  action->status = status;
}

static void sender_request_read(struct rdt_sender *sender,
                                struct rdt_protocol_actions *actions)
{
  uint64_t offset;
  uint64_t remaining;
  size_t length;
  struct rdt_protocol_action *action;

  if (sender->read_pending != 0 || sender->next_sequence >= sender->total_packets ||
      sender->next_sequence - sender->base >= sender->window_size)
  {
    return;
  }
  offset = sender->next_sequence * (uint64_t) RDT_MAX_PAYLOAD;
  remaining = sender->file_size - offset;
  length = remaining > (uint64_t) RDT_MAX_PAYLOAD
               ? (size_t) RDT_MAX_PAYLOAD
               : (size_t) remaining;
  action = add_action(actions, RDT_ACTION_READ_FILE);
  action->length = length;
  sender->read_pending = 1;
}

static void sender_wait_or_read(struct rdt_sender *sender, uint64_t now_ms,
                                struct rdt_protocol_actions *actions)
{
  if (sender->base == sender->total_packets)
  {
    sender->finishing = 1;
    sender->timer_active = 1;
    sender->deadline_ms = now_ms + (uint64_t) sender->timeout_ms;
    add_packet(actions, RDT_FIN, (uint32_t) sender->total_packets, NULL, 0U);
    add_wait(actions, sender->deadline_ms);
    return;
  }
  sender_request_read(sender, actions);
  if (sender->timer_active != 0)
  {
    add_wait(actions, sender->deadline_ms);
  }
}

int rdt_sender_init(struct rdt_sender *sender, uint64_t file_size,
                    unsigned int window_size, unsigned int timeout_ms,
                    uint64_t now_ms, struct rdt_protocol_actions *actions)
{
  if (sender == NULL || actions == NULL || window_size == 0U ||
      window_size > 64U || timeout_ms == 0U || timeout_ms > 60000U)
  {
    return -1;
  }
  memset(sender, 0, sizeof(*sender));
  clear_actions(actions);
  sender->file_size = file_size;
  sender->total_packets =
      file_size / (uint64_t) RDT_MAX_PAYLOAD +
      (file_size % (uint64_t) RDT_MAX_PAYLOAD == 0U ? 0U : 1U);
  if (sender->total_packets >= (uint64_t) UINT32_MAX)
  {
    return -1;
  }
  sender->window_size = window_size;
  sender->timeout_ms = timeout_ms;
  if (sender->total_packets == 0U)
  {
    sender_wait_or_read(sender, now_ms, actions);
    return 0;
  }
  sender_request_read(sender, actions);
  return 0;
}

static void sender_emit_outstanding(struct rdt_sender *sender,
                                    struct rdt_protocol_actions *actions)
{
  uint64_t sequence;

  for (sequence = sender->base; sequence < sender->next_sequence; ++sequence)
  {
    struct rdt_sender_slot *slot =
        &sender->slots[sequence % (uint64_t) sender->window_size];
    struct rdt_protocol_action *action =
        add_action(actions, RDT_ACTION_SEND_PACKET);
    memcpy(action->bytes, slot->bytes, slot->length);
    action->length = slot->length;
  }
}

int rdt_sender_step(struct rdt_sender *sender,
                    const struct rdt_sender_event *event,
                    struct rdt_protocol_actions *actions)
{
  if (sender == NULL || event == NULL || actions == NULL)
  {
    return -1;
  }
  clear_actions(actions);
  if (sender->done != 0)
  {
    add_status_action(actions, RDT_ACTION_COMPLETE, 0);
    return 0;
  }
  if (event->type == RDT_SENDER_EVENT_IO_FAILURE)
  {
    sender->done = 1;
    add_status_action(actions, RDT_ACTION_FAILED, 1);
    return 0;
  }

  if (sender->finishing != 0)
  {
    if (event->type == RDT_SENDER_EVENT_ACK &&
        event->packet.type == RDT_ACK &&
        event->packet.payload_length == 0U &&
        (uint64_t) event->packet.sequence == sender->total_packets + 1U)
    {
      sender->done = 1;
      add_status_action(actions, RDT_ACTION_COMPLETE, 0);
      return 0;
    }
    if (event->type == RDT_SENDER_EVENT_TIMER &&
        event->now_ms >= sender->deadline_ms)
    {
      ++sender->consecutive_timeouts;
      if (sender->consecutive_timeouts >= FIN_MAX_TIMEOUTS)
      {
        sender->done = 1;
        add_status_action(actions, RDT_ACTION_FAILED, 2);
        return 0;
      }
      sender->deadline_ms = event->now_ms + (uint64_t) sender->timeout_ms;
      add_packet(actions, RDT_FIN, (uint32_t) sender->total_packets, NULL, 0U);
    }
    add_wait(actions, sender->deadline_ms);
    return 0;
  }

  if (event->type == RDT_SENDER_EVENT_DATA_READY)
  {
    uint64_t offset;
    uint64_t remaining;
    size_t expected_length;
    struct rdt_sender_slot *slot;

    if (sender->read_pending == 0 || event->bytes == NULL)
    {
      return -1;
    }
    offset = sender->next_sequence * (uint64_t) RDT_MAX_PAYLOAD;
    remaining = sender->file_size - offset;
    expected_length = remaining > (uint64_t) RDT_MAX_PAYLOAD
                          ? (size_t) RDT_MAX_PAYLOAD
                          : (size_t) remaining;
    if (event->length != expected_length)
    {
      sender->done = 1;
      add_status_action(actions, RDT_ACTION_FAILED, 1);
      return 0;
    }
    slot = &sender->slots[sender->next_sequence %
                          (uint64_t) sender->window_size];
    slot->length = rdt_encode_packet(
        slot->bytes, sizeof(slot->bytes), RDT_DATA,
        (uint32_t) sender->next_sequence, event->bytes,
        (uint32_t) event->length);
    sender->read_pending = 0;
    add_packet(actions, RDT_DATA, (uint32_t) sender->next_sequence,
               event->bytes, (uint32_t) event->length);
    ++sender->next_sequence;
    if (sender->timer_active == 0)
    {
      sender->timer_active = 1;
      sender->deadline_ms = event->now_ms + (uint64_t) sender->timeout_ms;
    }
    sender_wait_or_read(sender, event->now_ms, actions);
    return 0;
  }

  if (event->type == RDT_SENDER_EVENT_ACK &&
      event->packet.type == RDT_ACK &&
      event->packet.payload_length == 0U &&
      (uint64_t) event->packet.sequence > sender->base &&
      (uint64_t) event->packet.sequence <= sender->next_sequence)
  {
    sender->base = event->packet.sequence;
    sender->consecutive_timeouts = 0U;
    if (sender->base < sender->next_sequence)
    {
      sender->timer_active = 1;
      sender->deadline_ms = event->now_ms + (uint64_t) sender->timeout_ms;
    }
    else
    {
      sender->timer_active = 0;
    }
    sender_wait_or_read(sender, event->now_ms, actions);
    return 0;
  }

  if (event->type == RDT_SENDER_EVENT_TIMER &&
      sender->timer_active != 0 && event->now_ms >= sender->deadline_ms)
  {
    ++sender->consecutive_timeouts;
    if (sender->consecutive_timeouts >= DATA_MAX_TIMEOUTS)
    {
      sender->done = 1;
      add_status_action(actions, RDT_ACTION_FAILED, 2);
      return 0;
    }
    sender_emit_outstanding(sender, actions);
    sender->deadline_ms = event->now_ms + (uint64_t) sender->timeout_ms;
  }
  sender_wait_or_read(sender, event->now_ms, actions);
  return 0;
}

static void receiver_ack(struct rdt_receiver *receiver,
                         struct rdt_protocol_actions *actions)
{
  add_packet(actions, RDT_ACK, (uint32_t) receiver->expected, NULL, 0U);
}

int rdt_receiver_init(struct rdt_receiver *receiver, uint64_t now_ms,
                      struct rdt_protocol_actions *actions)
{
  if (receiver == NULL || actions == NULL)
  {
    return -1;
  }
  memset(receiver, 0, sizeof(*receiver));
  clear_actions(actions);
  receiver->phase = RDT_RECEIVER_ACTIVE;
  receiver->idle_deadline_ms = now_ms + RECEIVE_IDLE_TIMEOUT_MS;
  add_wait(actions, receiver->idle_deadline_ms);
  return 0;
}

int rdt_receiver_step(struct rdt_receiver *receiver,
                      const struct rdt_receiver_event *event,
                      struct rdt_protocol_actions *actions)
{
  if (receiver == NULL || event == NULL || actions == NULL)
  {
    return -1;
  }
  clear_actions(actions);
  if (event->type == RDT_RECEIVER_EVENT_IO_FAILURE)
  {
    receiver->phase = RDT_RECEIVER_FAILED;
    add_status_action(actions, RDT_ACTION_FAILED, 1);
    return 0;
  }
  if (receiver->phase == RDT_RECEIVER_DONE)
  {
    add_status_action(actions, RDT_ACTION_COMPLETE, 0);
    return 0;
  }
  if (receiver->phase == RDT_RECEIVER_FAILED)
  {
    add_status_action(actions, RDT_ACTION_FAILED, 1);
    return 0;
  }
  if (receiver->phase == RDT_RECEIVER_ACTIVE &&
      event->type == RDT_RECEIVER_EVENT_TIMER &&
      event->now_ms >= receiver->idle_deadline_ms)
  {
    receiver->phase = RDT_RECEIVER_FAILED;
    add_status_action(actions, RDT_ACTION_FAILED, 2);
    return 0;
  }
  if (receiver->phase == RDT_RECEIVER_LINGER &&
      event->type == RDT_RECEIVER_EVENT_TIMER &&
      event->now_ms >= receiver->linger_deadline_ms)
  {
    receiver->phase = RDT_RECEIVER_DONE;
    add_status_action(actions, RDT_ACTION_COMPLETE, 0);
    return 0;
  }

  if (event->type == RDT_RECEIVER_EVENT_PACKET &&
      receiver->phase == RDT_RECEIVER_ACTIVE)
  {
    receiver->idle_deadline_ms = event->now_ms + RECEIVE_IDLE_TIMEOUT_MS;
    if (event->packet.type == RDT_DATA)
    {
      if ((uint64_t) event->packet.sequence != receiver->expected ||
          event->packet.payload_length == 0U ||
          event->packet.payload_length > RDT_MAX_PAYLOAD)
      {
        receiver_ack(receiver, actions);
      }
      else
      {
        memcpy(receiver->pending_payload, event->packet.payload,
               event->packet.payload_length);
        receiver->pending_length = event->packet.payload_length;
        receiver->phase = RDT_RECEIVER_WRITING;
        struct rdt_protocol_action *action =
            add_action(actions, RDT_ACTION_WRITE_FILE);
        memcpy(action->bytes, receiver->pending_payload,
               receiver->pending_length);
        action->length = receiver->pending_length;
      }
    }
    else if (event->packet.type == RDT_FIN)
    {
      if ((uint64_t) event->packet.sequence != receiver->expected)
      {
        receiver_ack(receiver, actions);
      }
      else
      {
        receiver->fin_sequence = event->packet.sequence;
        receiver->phase = RDT_RECEIVER_FLUSHING;
        (void) add_action(actions, RDT_ACTION_FLUSH_FILE);
      }
    }
    if (receiver->phase == RDT_RECEIVER_ACTIVE)
    {
      add_wait(actions, receiver->idle_deadline_ms);
    }
    return 0;
  }

  if (receiver->phase == RDT_RECEIVER_LINGER &&
      event->type == RDT_RECEIVER_EVENT_PACKET)
  {
    if (event->packet.type == RDT_FIN &&
        event->packet.sequence == receiver->fin_sequence)
    {
      receiver->expected = (uint64_t) receiver->fin_sequence + 1U;
      receiver_ack(receiver, actions);
    }
    add_wait(actions, receiver->linger_deadline_ms);
    return 0;
  }

  if (event->type == RDT_RECEIVER_EVENT_FILE_OK)
  {
    if (receiver->phase == RDT_RECEIVER_WRITING)
    {
      ++receiver->expected;
      receiver->phase = RDT_RECEIVER_ACTIVE;
      receiver->idle_deadline_ms = event->now_ms + RECEIVE_IDLE_TIMEOUT_MS;
      receiver_ack(receiver, actions);
      add_wait(actions, receiver->idle_deadline_ms);
      return 0;
    }
    if (receiver->phase == RDT_RECEIVER_FLUSHING)
    {
      receiver->phase = RDT_RECEIVER_CLOSING;
      (void) add_action(actions, RDT_ACTION_CLOSE_FILE);
      return 0;
    }
    if (receiver->phase == RDT_RECEIVER_CLOSING)
    {
      receiver->expected = (uint64_t) receiver->fin_sequence + 1U;
      receiver->phase = RDT_RECEIVER_LINGER;
      receiver->linger_deadline_ms = event->now_ms + FIN_LINGER_MS;
      receiver_ack(receiver, actions);
      add_wait(actions, receiver->linger_deadline_ms);
      return 0;
    }
  }

  if (event->type == RDT_RECEIVER_EVENT_TIMER)
  {
    add_wait(actions, receiver->phase == RDT_RECEIVER_LINGER
                          ? receiver->linger_deadline_ms
                          : receiver->idle_deadline_ms);
  }
  return 0;
}

#define _POSIX_C_SOURCE 200809L

#include "rdt_protocol.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define FIN_MAX_TIMEOUTS 10U
#define FIN_LINGER_EXTRA_MS UINT64_C(1000)

struct received_packet
{
  enum rdt_packet_type type;
  uint32_t sequence;
  uint8_t bytes[RDT_MAX_PACKET];
  const uint8_t *payload;
  uint32_t payload_length;
};

struct send_slot
{
  uint8_t bytes[RDT_MAX_PACKET];
  size_t length;
};

static uint16_t read_u16(const uint8_t *bytes)
{
  uint16_t value;
  memcpy(&value, bytes, sizeof(value));
  return ntohs(value);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
  uint16_t network_value = htons(value);
  memcpy(bytes, &network_value, sizeof(network_value));
}

static int protocol_io_is_valid(const struct rdt_protocol_io *io)
{
  return io != NULL && io->send_datagram != NULL &&
         io->receive_datagram != NULL && io->now_ms != NULL &&
         io->read_file != NULL && io->write_file != NULL &&
         io->flush_file != NULL;
}

static int receive_packet(const struct rdt_protocol_io *io, int timeout_ms,
                          struct received_packet *received)
{
  uint64_t start = io->now_ms(io->context);

  for (;;)
  {
    int remaining = timeout_ms;
    size_t length = 0U;
    int result;

    if (timeout_ms >= 0)
    {
      uint64_t now = io->now_ms(io->context);
      uint64_t elapsed = now >= start ? now - start : 0U;
      if (elapsed >= (uint64_t) timeout_ms)
      {
        return 1;
      }
      remaining = (int) ((uint64_t) timeout_ms - elapsed);
    }

    result = io->receive_datagram(io->context, received->bytes,
                                  sizeof(received->bytes), &length, remaining);
    if (result != 0)
    {
      return result;
    }
    if (rdt_decode_packet(received->bytes, length, &received->type,
                          &received->sequence, NULL,
                          &received->payload_length) == 0)
    {
      received->payload = received->bytes + RDT_HEADER_SIZE;
      return 0;
    }
  }
}

static int send_packet(const struct rdt_protocol_io *io,
                       enum rdt_packet_type type, uint32_t sequence,
                       const uint8_t *payload, uint32_t payload_length)
{
  uint8_t packet[RDT_MAX_PACKET];
  size_t length = rdt_encode_packet(packet, sizeof(packet), type, sequence,
                                    payload, payload_length);

  if (length == 0U)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not encode RDT packet\n");
    return -1;
  } // GCOVR_EXCL_STOP
  return io->send_datagram(io->context, packet, length);
}

static int send_finish(const struct rdt_protocol_io *io,
                       uint32_t packet_count, unsigned int timeout_ms)
{
  uint8_t payload[2];
  uint32_t finish_ack = packet_count + 1U;
  uint64_t next_send = 0U;
  unsigned int timeouts = 0U;

  write_u16(payload, (uint16_t) timeout_ms);
  for (;;)
  {
    struct received_packet received;
    uint64_t now = io->now_ms(io->context);
    int wait_ms;
    int result;

    if (now >= next_send)
    {
      if (send_packet(io, RDT_FIN, packet_count, payload,
                      (uint32_t) sizeof(payload)) != 0)
      {
        return -1;
      }
      next_send = io->now_ms(io->context) + (uint64_t) timeout_ms;
    }
    now = io->now_ms(io->context);
    wait_ms = now >= next_send ? 0 : (int) (next_send - now);
    result = receive_packet(io, wait_ms, &received);
    if (result < 0)
    { // GCOVR_EXCL_START
      return -1;
    } // GCOVR_EXCL_STOP
    if (result == 1)
    {
      ++timeouts;
      if (timeouts >= FIN_MAX_TIMEOUTS)
      {
        fprintf(stderr, "FIN was not acknowledged after %u timeouts\n",
                FIN_MAX_TIMEOUTS);
        return 2;
      }
    }
    if (result == 0 && received.type == RDT_ACK &&
        received.sequence == finish_ack && received.payload_length == 0U)
    {
      return 0;
    }
  }
}

static int fill_send_window(const struct rdt_protocol_io *io,
                            uint64_t file_size, uint64_t total_packets,
                            uint64_t *next_sequence, uint64_t base,
                            unsigned int window_size,
                            struct send_slot *slots)
{
  while (*next_sequence < total_packets &&
         *next_sequence - base < (uint64_t) window_size)
  {
    uint64_t offset = *next_sequence * (uint64_t) RDT_MAX_PAYLOAD;
    uint64_t remaining = file_size - offset;
    uint32_t chunk_length = remaining > (uint64_t) RDT_MAX_PAYLOAD
                              ? RDT_MAX_PAYLOAD
                              : (uint32_t) remaining;
    struct send_slot *slot = &slots[*next_sequence % (uint64_t) window_size];
    size_t packet_length;

    if (io->read_file(io->context, slot->bytes + RDT_HEADER_SIZE,
                      (size_t) chunk_length) != 0)
    { // GCOVR_EXCL_START
      fprintf(stderr, "Could not read expected bytes from input file\n");
      return -1;
    } // GCOVR_EXCL_STOP
    packet_length = rdt_encode_packet(slot->bytes, sizeof(slot->bytes),
                                      RDT_DATA, (uint32_t) *next_sequence,
                                      slot->bytes + RDT_HEADER_SIZE,
                                      chunk_length);
    if (packet_length == 0U)
    { // GCOVR_EXCL_START
      fprintf(stderr, "Could not encode data packet\n");
      return -1;
    } // GCOVR_EXCL_STOP
    slot->length = packet_length;
    if (io->send_datagram(io->context, slot->bytes, slot->length) != 0)
    { // GCOVR_EXCL_START
      return -1;
    } // GCOVR_EXCL_STOP
    ++*next_sequence;
  }
  return 0;
}

int rdt_protocol_send(const struct rdt_protocol_io *io, uint64_t file_size,
                      const struct rdt_options *options)
{
  struct send_slot slots[64];
  uint64_t total_packets;
  uint64_t base = 0U;
  uint64_t next_sequence = 0U;
  uint64_t deadline = 0U;
  int timer_active = 0;

  if (!protocol_io_is_valid(io) || options == NULL ||
      options->window_size == 0U || options->window_size > 64U ||
      options->timeout_ms == 0U || options->timeout_ms > 60000U)
  {
    fprintf(stderr, "Invalid protocol I/O or sender options\n");
    return 1;
  }

  total_packets = file_size / (uint64_t) RDT_MAX_PAYLOAD +
                  (file_size % (uint64_t) RDT_MAX_PAYLOAD == 0U ? 0U : 1U);
  if (total_packets >= (uint64_t) UINT32_MAX)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Input is too large for the 32-bit sequence space\n");
    return 1;
  } // GCOVR_EXCL_STOP

  /*
   * base is the oldest unacknowledged DATA sequence and next_sequence is the
   * next chunk to send. ACKs advance base cumulatively; on timeout all packets
   * from base through next_sequence - 1 are retransmitted.
   */
  while (base < total_packets)
  {
    struct received_packet received;
    uint64_t old_next = next_sequence;
    uint64_t now;
    int wait_ms;
    int packet_result;

    if (fill_send_window(io, file_size, total_packets, &next_sequence, base,
                         options->window_size, slots) != 0)
    { // GCOVR_EXCL_START
      return 1;
    } // GCOVR_EXCL_STOP
    if (next_sequence > old_next && timer_active == 0)
    {
      deadline = io->now_ms(io->context) + (uint64_t) options->timeout_ms;
      timer_active = 1;
    }

    now = io->now_ms(io->context);
    wait_ms = now >= deadline ? 0 : (int) (deadline - now);
    packet_result = receive_packet(io, wait_ms, &received);
    if (packet_result < 0)
    { // GCOVR_EXCL_START
      return 1;
    } // GCOVR_EXCL_STOP
    if (packet_result == 1)
    {
      uint64_t sequence;
      for (sequence = base; sequence < next_sequence; ++sequence)
      {
        struct send_slot *slot =
            &slots[sequence % (uint64_t) options->window_size];
        if (io->send_datagram(io->context, slot->bytes, slot->length) != 0)
        { // GCOVR_EXCL_START
          return 1;
        } // GCOVR_EXCL_STOP
      }
      deadline = io->now_ms(io->context) + (uint64_t) options->timeout_ms;
      timer_active = 1;
      continue;
    }

    if (received.type == RDT_ACK && received.payload_length == 0U &&
        (uint64_t) received.sequence > base &&
        (uint64_t) received.sequence <= next_sequence)
    {
      base = received.sequence;
      if (base < next_sequence)
      {
        deadline = io->now_ms(io->context) +
                   (uint64_t) options->timeout_ms;
        timer_active = 1;
      }
      else
      {
        timer_active = 0;
      }
    }
  }

  return send_finish(io, (uint32_t) total_packets, options->timeout_ms);
}

static int send_ack(const struct rdt_protocol_io *io, uint32_t sequence)
{
  return send_packet(io, RDT_ACK, sequence, NULL, 0U);
}

static int receive_linger(const struct rdt_protocol_io *io,
                          uint32_t packet_count, unsigned int timeout_ms)
{
  uint32_t finish_ack = packet_count + 1U;
  uint64_t deadline = io->now_ms(io->context) +
                      (uint64_t) timeout_ms * (uint64_t) FIN_MAX_TIMEOUTS +
                      FIN_LINGER_EXTRA_MS;

  /* Re-ACK duplicate FINs for the full FIN retransmission period. */
  while (io->now_ms(io->context) < deadline)
  {
    struct received_packet received;
    uint64_t now = io->now_ms(io->context);
    int remaining = (int) (deadline - now);
    int result = receive_packet(io, remaining, &received);

    if (result < 0)
    {
      return -1;
    }
    if (result == 1)
    {
      return 0;
    }
    if (received.type == RDT_FIN && received.sequence == packet_count &&
        received.payload_length == 2U &&
        read_u16(received.payload) == timeout_ms)
    {
      if (send_ack(io, finish_ack) != 0)
      { // GCOVR_EXCL_START
        return -1;
      } // GCOVR_EXCL_STOP
    }
    else if (received.type == RDT_DATA && received.sequence < packet_count)
    {
      if (send_ack(io, packet_count) != 0)
      { // GCOVR_EXCL_START
        return -1;
      } // GCOVR_EXCL_STOP
    }
  } // GCOVR_EXCL_START
  return 0;
} // GCOVR_EXCL_STOP

int rdt_protocol_receive(const struct rdt_protocol_io *io,
                         const struct rdt_options *options)
{
  uint64_t expected = 0U;
  uint32_t packet_count;
  unsigned int sender_timeout;

  if (!protocol_io_is_valid(io) || options == NULL)
  {
    fprintf(stderr, "Invalid protocol I/O or receiver options\n");
    return 1;
  }

  for (;;)
  {
    struct received_packet received;
    int receive_result = receive_packet(io, -1, &received);

    if (receive_result < 0)
    {
      return 1;
    }
    if (received.type == RDT_DATA)
    {
      if ((uint64_t) received.sequence == expected &&
          received.payload_length > 0U &&
          received.payload_length <= RDT_MAX_PAYLOAD)
      {
        if (io->write_file(io->context, received.payload,
                           (size_t) received.payload_length) != 0)
        { // GCOVR_EXCL_START
          fprintf(stderr, "Could not write output file\n");
          return 1;
        } // GCOVR_EXCL_STOP
        ++expected;
      }
      if (send_ack(io, (uint32_t) expected) != 0)
      { // GCOVR_EXCL_START
        return 1;
      } // GCOVR_EXCL_STOP
    }
    else if (received.type == RDT_FIN)
    {
      if ((uint64_t) received.sequence != expected ||
          received.payload_length != 2U)
      { // GCOVR_EXCL_START
        if (send_ack(io, (uint32_t) expected) != 0)
        {
          return 1;
        }
        continue;
      } // GCOVR_EXCL_STOP
      sender_timeout = read_u16(received.payload);
      if (sender_timeout == 0U || sender_timeout > 60000U)
      { // GCOVR_EXCL_START
        if (send_ack(io, (uint32_t) expected) != 0)
        {
          return 1;
        }
        continue;
      } // GCOVR_EXCL_STOP
      if (io->flush_file(io->context) != 0)
      { // GCOVR_EXCL_START
        fprintf(stderr, "Could not flush output file\n");
        return 1;
      } // GCOVR_EXCL_STOP
      packet_count = (uint32_t) expected;
      if (send_ack(io, packet_count + 1U) != 0)
      { // GCOVR_EXCL_START
        return 1;
      } // GCOVR_EXCL_STOP
      return receive_linger(io, packet_count, sender_timeout);
    }
  }
}

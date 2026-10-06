#include "rdt.h"

#include "rdt_io.h"
#include "rdt_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Packet serialization layer. It owns the wire format and does not perform
 * network, timer, or file operations.
 */
static uint16_t read_u16(const uint8_t *bytes)
{
  uint16_t network_value;
  memcpy(&network_value, bytes, sizeof(network_value));
  return ntohs(network_value);
}

static uint32_t read_u32(const uint8_t *bytes)
{
  uint32_t network_value;
  memcpy(&network_value, bytes, sizeof(network_value));
  return ntohl(network_value);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
  uint16_t network_value = htons(value);
  memcpy(bytes, &network_value, sizeof(network_value));
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
  uint32_t network_value = htonl(value);
  memcpy(bytes, &network_value, sizeof(network_value));
}

/* Internet checksum with the checksum field temporarily treated as zero. */
static uint16_t internet_checksum(const uint8_t *packet, size_t length,
                                 int zero_checksum)
{
  uint32_t sum = 0U;
  size_t offset = 0U;

  while (offset + 1U < length)
  {
    uint16_t word;
    if (zero_checksum != 0 && offset == 2U)
    {
      word = 0U;
    }
    else
    {
      word = (uint16_t) (((uint16_t) packet[offset] << 8) |
                         (uint16_t) packet[offset + 1U]);
    }
    sum += (uint32_t) word;
    offset += 2U;
  }
  if (offset < length)
  {
    sum += (uint32_t) ((uint16_t) packet[offset] << 8);
  }
  while ((sum >> 16) != 0U)
  {
    sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
  }
  return (uint16_t) ~sum;
}

int rdt_validate_session(const char *session)
{
  size_t i;
  size_t length;

  if (session == NULL)
  {
    return -1;
  }
  length = strlen(session);
  if (length == 0U || length > 32U)
  {
    return -1;
  }
  for (i = 0U; i < length; ++i)
  {
    char character = session[i];
    if (!((character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-'))
    {
      return -1;
    }
  }
  return 0;
}

size_t rdt_encode_packet(uint8_t *packet, size_t capacity,
                         enum rdt_packet_type type, uint32_t sequence,
                         const uint8_t *payload, uint32_t payload_length)
{
  size_t packet_length;

  if (packet == NULL || payload_length > RDT_MAX_PAYLOAD ||
      (payload_length > 0U && payload == NULL) ||
      (type != RDT_DATA && type != RDT_ACK && type != RDT_FIN))
  {
    return 0U;
  }
  packet_length = RDT_HEADER_SIZE + (size_t) payload_length;
  if (capacity < packet_length)
  {
    return 0U;
  }

  packet[0] = (uint8_t) type;
  packet[1] = 0U;
  write_u16(packet + 2U, 0U);
  write_u32(packet + 4U, sequence);
  write_u16(packet + 8U, (uint16_t) payload_length);
  if (payload_length > 0U)
  {
    memcpy(packet + RDT_HEADER_SIZE, payload, (size_t) payload_length);
  }
  write_u16(packet + 2U, internet_checksum(packet, packet_length, 1));
  return packet_length;
}

int rdt_decode_packet(const uint8_t *packet, size_t packet_length,
                      enum rdt_packet_type *type, uint32_t *sequence,
                      const uint8_t **payload, uint32_t *payload_length)
{
  uint16_t decoded_length;

  if (packet == NULL || packet_length < RDT_HEADER_SIZE || packet[1] != 0U ||
      (packet[0] != RDT_DATA && packet[0] != RDT_ACK &&
       packet[0] != RDT_FIN))
  {
    return -1;
  }

  decoded_length = read_u16(packet + 8U);
  if (decoded_length > RDT_MAX_PAYLOAD ||
      packet_length != RDT_HEADER_SIZE + (size_t) decoded_length ||
      internet_checksum(packet, packet_length, 0) != 0U)
  {
    return -1;
  }

  if (type != NULL)
  {
    *type = (enum rdt_packet_type) packet[0];
  }
  if (sequence != NULL)
  {
    *sequence = read_u32(packet + 4U);
  }
  if (payload != NULL)
  {
    *payload = packet + RDT_HEADER_SIZE;
  }
  if (payload_length != NULL)
  {
    *payload_length = decoded_length;
  }
  return 0;
}

/*
 * Public entry points compose the I/O adapter with the protocol state machine.
 */
int rdt_send_file(const char *host, unsigned short port, const char *path,
                  const struct rdt_options *options)
{
  struct rdt_io_context *context;
  struct rdt_protocol_io io;
  uint64_t file_size = 0U;
  int result;
  int close_result;

  context = rdt_io_open(host, port, path, options, 1, &file_size);
  if (context == NULL)
  {
    return 1;
  }
  rdt_io_get_protocol_io(context, &io);
  result = rdt_protocol_send(&io, file_size, options);
  close_result = rdt_io_close(context);

  if (result == 0 && close_result == 0)
  {
    printf("Transfer complete: %" PRIu64 " bytes sent to %s\n", file_size,
           host);
    return 0;
  }
  return result == 2 ? 2 : 1;
}

int rdt_receive_file(const char *host, unsigned short port, const char *path,
                     const struct rdt_options *options)
{
  struct rdt_io_context *context;
  struct rdt_protocol_io io;
  int result;
  int close_result;

  context = rdt_io_open(host, port, path, options, 0, NULL);
  if (context == NULL)
  {
    return 1;
  }
  rdt_io_get_protocol_io(context, &io);
  result = rdt_protocol_receive(&io, options);
  close_result = rdt_io_close(context);

  if (result == 0 && close_result == 0)
  {
    printf("Transfer complete: file received from %s\n", host);
    return 0;
  }
  // GCOVR_EXCL_START
  return result == 0 ? 0 : 1;
  // GCOVR_EXCL_STOP
}

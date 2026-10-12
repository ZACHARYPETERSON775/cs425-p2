#include "rdt.h"

#include <arpa/inet.h>
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

static uint16_t checksum_bytes(const uint8_t *bytes, size_t length,
                               int zero_checksum_field)
{
  uint32_t sum = 0U;
  size_t offset = 0U;

  if (bytes == NULL && length > 0U)
  {
    return 0U;
  }
  while (offset + 1U < length)
  {
    uint16_t word = zero_checksum_field != 0 && offset == 2U
                        ? 0U
                        : (uint16_t) (((uint16_t) bytes[offset] << 8) |
                                      (uint16_t) bytes[offset + 1U]);
    sum += (uint32_t) word;
    offset += 2U;
  }
  if (offset < length)
  {
    sum += (uint32_t) ((uint16_t) bytes[offset] << 8);
  }
  while ((sum >> 16) != 0U)
  {
    sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
  }
  return (uint16_t) ~sum;
}

uint16_t rdt_checksum(const uint8_t *bytes, size_t length)
{
  return checksum_bytes(bytes, length, 0);
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
  write_u16(packet + 2U, checksum_bytes(packet, packet_length, 1));
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
      rdt_checksum(packet, packet_length) != 0U)
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

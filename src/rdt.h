#ifndef RDT_H
#define RDT_H

#include <stddef.h>
#include <stdint.h>

/** Size in bytes of the fixed portion of an RDT datagram. */
#define RDT_HEADER_SIZE 10U

/** Maximum number of file-data bytes carried by one DATA datagram. */
#define RDT_MAX_PAYLOAD 1024U

/** Maximum total RDT datagram size, including its header. */
#define RDT_MAX_PACKET (RDT_HEADER_SIZE + RDT_MAX_PAYLOAD)

/**
 * RDT datagram type values. These values are part of the wire protocol.
 */
enum rdt_packet_type
{
  RDT_DATA = 0,
  RDT_ACK = 1,
  RDT_FIN = 2
};

/**
 * Configuration for a transfer and its relay registration.
 *
 * The session string is used during the relay HELLO handshake. window_size and
 * timeout_ms are used by the sender; loss, corrupt, and duplicate are sent to
 * the relay in the sender's HELLO. port selects the relay's UDP port.
 */
struct rdt_options
{
  const char *session;       /**< Shared relay session name. */
  unsigned int window_size;  /**< Sender's Go-Back-N window, 1-64 packets. */
  unsigned int timeout_ms;   /**< Sender retransmission timeout in ms. */
  unsigned short port;       /**< Relay UDP port. */
  double loss;               /**< Relay packet-loss probability, 0-0.5. */
  double corrupt;            /**< Relay packet-corruption probability, 0-0.5. */
  double duplicate;          /**< Relay duplication probability, 0-0.5. */
};

/**
 * Check the session-name syntax expected by the relay.
 *
 * @param session Name to validate; must contain 1-32 lowercase letters,
 *                digits, or hyphens.
 * @return 0 when valid, or -1 for NULL or an invalid name.
 */
int rdt_validate_session(const char *session);

/**
 * Encode one RDT datagram into its wire representation.
 *
 * The header consists of type, a zero reserved byte, a 16-bit Internet
 * checksum, a 32-bit sequence number, and a 16-bit payload length. Multi-byte
 * fields are in network byte order.
 *
 * @param packet Destination buffer.
 * @param capacity Available bytes in packet.
 * @param type DATA, ACK, or FIN.
 * @param sequence 32-bit datagram sequence number.
 * @param payload Payload bytes, or NULL when payload_length is zero.
 * @param payload_length Payload size, at most RDT_MAX_PAYLOAD.
 * @return Encoded datagram length, or 0 if the arguments/capacity are invalid.
 */
size_t rdt_encode_packet(uint8_t *packet, size_t capacity,
                         enum rdt_packet_type type, uint32_t sequence,
                         const uint8_t *payload, uint32_t payload_length);

/**
 * Validate and decode an RDT datagram.
 *
 * Optional output pointers may be NULL. The decoded payload points into the
 * input packet and remains valid only as long as that packet's storage.
 *
 * @param packet Datagram bytes to validate.
 * @param packet_length Datagram size.
 * @param type Receives the decoded type, when non-NULL.
 * @param sequence Receives the decoded sequence number, when non-NULL.
 * @param payload Receives a pointer to the payload, when non-NULL.
 * @param payload_length Receives the payload size, when non-NULL.
 * @return 0 on success, or -1 for invalid type, reserved byte, length, or
 *         checksum.
 */
int rdt_decode_packet(const uint8_t *packet, size_t packet_length,
                      enum rdt_packet_type *type, uint32_t *sequence,
                      const uint8_t **payload, uint32_t *payload_length);

/**
 * Send a regular file through the relay using Go-Back-N.
 *
 * Registers as the session sender, sends the file in RDT_MAX_PAYLOAD-sized
 * DATA chunks, retransmits unacknowledged windows on timeout, then retries FIN
 * until its final ACK arrives or ten FIN timeouts expire.
 *
 * @param host Relay host name or numeric address.
 * @param port Relay UDP port.
 * @param path Path to the regular input file.
 * @param options Transfer and relay settings.
 * @return 0 on success, 2 if FIN receives no ACK after ten retransmission
 *         timeouts, or 1 for other failures.
 */
int rdt_send_file(const char *host, unsigned short port, const char *path,
                  const struct rdt_options *options);

/**
 * Receive one file through the relay.
 *
 * Registers as the session receiver, writes only DATA at the next expected
 * sequence number, sends cumulative ACKs for received or discarded packets,
 * then acknowledges FIN and lingers through the sender's bounded FIN retry
 * period.
 *
 * @param host Relay host name or numeric address.
 * @param port Relay UDP port.
 * @param path Destination file path; it is opened/truncated after registration.
 * @param options Transfer and relay settings.
 * @return 0 on success, or nonzero if opening/writing the file, communicating
 *         with the relay, or completing the transfer fails.
 */
int rdt_receive_file(const char *host, unsigned short port, const char *path,
                     const struct rdt_options *options);

#endif

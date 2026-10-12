#include "harness/unity.h"
#include "../src/rdt.h"
#include "../src/rdt_io.h"
#include "../src/rdt_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RELAY_TIMEOUT_MS 8000

struct relay_test_state
{
  struct sockaddr_in receiver;
  struct sockaddr_in sender;
  int has_receiver;
  int has_sender;
  int drop_first_data;
  int corrupt_first_data;
  int duplicate_data;
  int drop_fin_ack;
  int drop_all_fin_acks;
  uint32_t fin_sequence;
};

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_session_validation(void)
{
  TEST_ASSERT_EQUAL_INT(0, rdt_validate_session("jdoe-1"));
  TEST_ASSERT_EQUAL_INT(0, rdt_validate_session("a"));
  TEST_ASSERT_EQUAL_INT(0, rdt_validate_session(
                               "abcdefghijklmnopqrstuvwxyz012345"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session(""));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session("Upper"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session("has space"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session("underscore_name"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session("{"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session(
                               "abcdefghijklmnopqrstuvwxyz0123456"));
  TEST_ASSERT_NOT_EQUAL(0, rdt_validate_session(NULL));
}

static void test_packet_round_trip(void)
{
  const uint8_t payload[] = {0U, 0x7fU, 0x80U, 0xffU};
  uint8_t packet[RDT_MAX_PACKET];
  const uint8_t *decoded_payload = NULL;
  enum rdt_packet_type type = RDT_ACK;
  uint32_t sequence = 0U;
  uint32_t payload_length = 0U;
  size_t packet_length = rdt_encode_packet(
      packet, sizeof(packet), RDT_DATA, UINT32_C(0x12345678),
      payload, sizeof(payload));

  TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE + sizeof(payload), packet_length);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[0]);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[1]);
  TEST_ASSERT_EQUAL_UINT8(0x12U, packet[4]);
  TEST_ASSERT_EQUAL_UINT8(0x34U, packet[5]);
  TEST_ASSERT_EQUAL_UINT8(0x56U, packet[6]);
  TEST_ASSERT_EQUAL_UINT8(0x78U, packet[7]);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[8]);
  TEST_ASSERT_EQUAL_UINT8(4U, packet[9]);
  TEST_ASSERT_EQUAL_INT(
      0, rdt_decode_packet(packet, packet_length, &type, &sequence,
                           &decoded_payload, &payload_length));
  TEST_ASSERT_EQUAL_INT(RDT_DATA, type);
  TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x12345678), sequence);
  TEST_ASSERT_EQUAL_UINT(sizeof(payload), payload_length);
  TEST_ASSERT_EQUAL_MEMORY(payload, decoded_payload, sizeof(payload));
}

static void test_rfc1071_checksum_vectors(void)
{
  const uint8_t rfc1071_even[] = {
    0x00U, 0x01U, 0xf2U, 0x03U, 0xf4U, 0xf5U, 0xf6U, 0xf7U
  };
  const uint8_t odd_length[] = {0x00U, 0x01U, 0xf2U, 0x03U, 0xf4U};

  TEST_ASSERT_EQUAL_HEX16(0x220dU,
                         rdt_checksum(rfc1071_even, sizeof(rfc1071_even)));
  TEST_ASSERT_EQUAL_HEX16(0x19faU,
                         rdt_checksum(odd_length, sizeof(odd_length)));
  TEST_ASSERT_EQUAL_HEX16(0U, rdt_checksum(NULL, 1U));
  TEST_ASSERT_EQUAL_HEX16(0xffffU, rdt_checksum(NULL, 0U));
  {
    const uint8_t carry_words[] = {0xffU, 0xffU, 0xffU, 0xffU};
    TEST_ASSERT_EQUAL_HEX16(0U,
                            rdt_checksum(carry_words, sizeof(carry_words)));
  }
}

static void test_packet_rejects_corruption_and_bad_lengths(void)
{
  const uint8_t payload[] = {1U, 2U, 3U};
  uint8_t packet[RDT_MAX_PACKET];
  size_t packet_length = rdt_encode_packet(packet, sizeof(packet), RDT_DATA,
                                           7U, payload, sizeof(payload));

  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length - 1U,
                                              NULL, NULL, NULL, NULL));
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length + 1U,
                                              NULL, NULL, NULL, NULL));
  packet[RDT_HEADER_SIZE + 1U] ^= 1U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
  packet[RDT_HEADER_SIZE + 1U] ^= 1U;
  packet[4] = 2U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
  packet[4] = 7U;
  packet[0] = 3U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
  packet[0] = (uint8_t) RDT_DATA;
  packet[1] = 1U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
  packet[1] = 0U;
  packet[8] = 4U;
  packet[9] = 1U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(NULL, packet_length, NULL, NULL,
                                              NULL, NULL));
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, RDT_HEADER_SIZE - 1U,
                                              NULL, NULL, NULL, NULL));
}

static void test_packet_odd_payload_and_single_bit_checksum_detection(void)
{
  const uint8_t odd_payload[] = {0x00U, 0x11U, 0x22U};
  uint8_t packet[RDT_MAX_PACKET];
  size_t packet_length = rdt_encode_packet(packet, sizeof(packet), RDT_DATA,
                                           42U, odd_payload,
                                           sizeof(odd_payload));

  TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE + sizeof(odd_payload), packet_length);
  TEST_ASSERT_EQUAL_INT(0, rdt_decode_packet(packet, packet_length, NULL, NULL,
                                              NULL, NULL));
  packet[packet_length - 1U] ^= 0x01U;
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(packet, packet_length, NULL,
                                              NULL, NULL, NULL));
}

static void test_ack_and_fin_type_values(void)
{
  const uint8_t finish_payload[] = {0x01U, 0xf4U};
  uint8_t packet[RDT_MAX_PACKET];
  enum rdt_packet_type type = RDT_DATA;
  uint32_t sequence = 0U;
  uint32_t payload_length = 0U;
  size_t packet_length;

  packet_length = rdt_encode_packet(packet, sizeof(packet), RDT_ACK,
                                   UINT32_C(0x01020304), NULL, 0U);
  TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE, packet_length);
  TEST_ASSERT_EQUAL_UINT8(1U, packet[0]);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[1]);
  TEST_ASSERT_EQUAL_INT(
      0, rdt_decode_packet(packet, packet_length, &type, &sequence, NULL,
                           &payload_length));
  TEST_ASSERT_EQUAL_INT(RDT_ACK, type);
  TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x01020304), sequence);
  TEST_ASSERT_EQUAL_UINT(0U, payload_length);

  packet_length = rdt_encode_packet(packet, sizeof(packet), RDT_FIN, 9U,
                                    finish_payload, sizeof(finish_payload));
  TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE + sizeof(finish_payload),
                         packet_length);
  TEST_ASSERT_EQUAL_UINT8(2U, packet[0]);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[1]);
  TEST_ASSERT_EQUAL_UINT8(0U, packet[8]);
  TEST_ASSERT_EQUAL_UINT8(2U, packet[9]);
  TEST_ASSERT_EQUAL_INT(
      0, rdt_decode_packet(packet, packet_length, &type, &sequence, NULL,
                           &payload_length));
  TEST_ASSERT_EQUAL_INT(RDT_FIN, type);
  TEST_ASSERT_EQUAL_UINT32(9U, sequence);
  TEST_ASSERT_EQUAL_UINT(sizeof(finish_payload), payload_length);
}

static void test_encode_rejects_oversized_or_missing_payload(void)
{
  uint8_t packet[RDT_MAX_PACKET];

  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(NULL, sizeof(packet), RDT_DATA, 0U, NULL, 0U));
  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(packet, sizeof(packet), RDT_DATA, 0U, NULL, 1U));
  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(packet, sizeof(packet), RDT_DATA, 0U, packet,
                            RDT_MAX_PAYLOAD + 1U));
  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(packet, 1U, RDT_DATA, 0U, NULL, 0U));
  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(packet, sizeof(packet), (enum rdt_packet_type) 3,
                            0U, NULL, 0U));
}

static int make_relay_socket(unsigned short *port)
{
  struct sockaddr_in address;
  socklen_t address_length = (socklen_t) sizeof(address);
  int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (socket_fd < 0)
  {
    return -1;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0U);
  if (bind(socket_fd, (struct sockaddr *) &address, sizeof(address)) != 0 ||
      getsockname(socket_fd, (struct sockaddr *) &address, &address_length) !=
          0)
  {
    (void) close(socket_fd);
    return -1;
  }
  *port = ntohs(address.sin_port);
  return socket_fd;
}

static int receive_relay_datagram(int socket_fd, uint8_t *packet,
                                 size_t capacity,
                                 struct sockaddr_in *source, int timeout_ms)
{
  struct pollfd descriptor = {
    .fd = socket_fd,
    .events = POLLIN,
    .revents = 0
  };
  socklen_t source_length = (socklen_t) sizeof(*source);
  int poll_result = poll(&descriptor, 1U, timeout_ms);
  ssize_t received;

  if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0)
  {
    return -1;
  }
  received = recvfrom(socket_fd, packet, capacity, 0,
                      (struct sockaddr *) source, &source_length);
  if (received < 0)
  {
    return -1;
  }
  return (int) received;
}

static int send_relay_datagram(int socket_fd, const uint8_t *packet,
                               size_t length,
                               const struct sockaddr_in *destination)
{
  ssize_t sent = sendto(socket_fd, packet, length, 0,
                        (const struct sockaddr *) destination,
                        (socklen_t) sizeof(*destination));
  return sent >= 0 && (size_t) sent == length ? 0 : -1;
}

static int same_address(const struct sockaddr_in *left,
                        const struct sockaddr_in *right)
{
  return left->sin_addr.s_addr == right->sin_addr.s_addr &&
         left->sin_port == right->sin_port;
}

static uint64_t test_monotonic_milliseconds(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  {
    return 0U;
  }
  return (uint64_t) now.tv_sec * UINT64_C(1000) +
         (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static void handle_test_relay_packet(int socket_fd,
                                     struct relay_test_state *state,
                                     const uint8_t *packet, size_t length,
                                     const struct sockaddr_in *source)
{
  uint8_t modified[RDT_MAX_PACKET];
  char hello[128];
  struct sockaddr_in *destination;

  if (length >= 6U && memcmp(packet, "HELLO ", 6U) == 0)
  {
    if (length >= sizeof(hello))
    {
      return;
    }
    memcpy(hello, packet, length);
    hello[length] = '\0';
    if (strstr(hello, " recv") != NULL)
    {
      state->receiver = *source;
      state->has_receiver = 1;
    }
    else if (strstr(hello, " send ") != NULL)
    {
      state->sender = *source;
      state->has_sender = 1;
    }
    (void) send_relay_datagram(socket_fd, (const uint8_t *) "OK", 2U, source);
    return;
  }

  if (state->has_sender != 0 && same_address(source, &state->sender))
  {
    enum rdt_packet_type type;
    uint32_t sequence;
    const uint8_t *payload;
    uint32_t payload_length;

    if (rdt_decode_packet(packet, length, &type, &sequence, &payload,
                          &payload_length) != 0)
    {
      return;
    }
    destination = &state->receiver;
    if (type == RDT_DATA && sequence == 0U && state->drop_first_data != 0)
    {
      state->drop_first_data = 0;
      return;
    }
    if (type == RDT_DATA && sequence == 1U &&
        state->corrupt_first_data != 0)
    {
      state->corrupt_first_data = 0;
      memcpy(modified, packet, length);
      modified[RDT_HEADER_SIZE] ^= 1U;
      (void) send_relay_datagram(socket_fd, modified, length, destination);
      return;
    }
    if (type == RDT_DATA && sequence == 2U && state->duplicate_data != 0)
    {
      state->duplicate_data = 0;
      (void) send_relay_datagram(socket_fd, packet, length, destination);
      (void) send_relay_datagram(socket_fd, packet, length, destination);
      return;
    }
    if (type == RDT_FIN)
    {
      state->fin_sequence = sequence;
    }
  }
  else if (state->has_receiver != 0 &&
           same_address(source, &state->receiver))
  {
    enum rdt_packet_type type;
    uint32_t sequence;

    if (rdt_decode_packet(packet, length, &type, &sequence, NULL, NULL) != 0)
    {
      return;
    }
    destination = &state->sender;
    if (type == RDT_ACK && sequence == state->fin_sequence + 1U &&
        (state->drop_fin_ack != 0 || state->drop_all_fin_acks != 0))
    {
      if (state->drop_all_fin_acks == 0)
      {
        state->drop_fin_ack = 0;
      }
      return;
    }
  }
  else
  {
    return;
  }

  if (state->has_receiver != 0 && state->has_sender != 0)
  {
    (void) send_relay_datagram(socket_fd, packet, length, destination);
  }
}

static int write_test_file(const char *path, const uint8_t *bytes,
                           size_t length)
{
  FILE *file = fopen(path, "wb");
  int result = 0;

  if (file == NULL)
  {
    return -1;
  }
  if (length > 0U && fwrite(bytes, 1U, length, file) != length)
  {
    result = -1;
  }
  if (fclose(file) != 0)
  {
    result = -1;
  }
  return result;
}

static void finish_child(pid_t *child, int *status)
{
  if (*child > 0)
  {
    (void) waitpid(*child, status, 0);
    *child = -1;
  }
}

static int run_transfer_case(const uint8_t *contents, size_t content_length,
                            const char *session, int inject_faults,
                            int drop_all_fin_acks)
{
  struct relay_test_state relay_state = {0};
  struct rdt_options options = {
    .session = session,
    .window_size = 4U,
    .timeout_ms = 50U,
    .port = 0U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  uint8_t datagram[RDT_MAX_PACKET];
  unsigned short relay_port = 0U;
  char input_path[] = "/tmp/cs425-rdt-input-XXXXXX";
  char output_path[] = "/tmp/cs425-rdt-output-XXXXXX";
  int input_fd = -1;
  int output_fd = -1;
  int relay_fd = -1;
  int sender_status = 0;
  int receiver_status = 0;
  int datagram_length;
  pid_t receiver_pid = -1;
  pid_t sender_pid = -1;
  uint64_t deadline;
  FILE *received_file = NULL;
  uint8_t *received_contents = NULL;
  size_t received_length = 0U;
  int result = -1;

  input_fd = mkstemp(input_path);
  output_fd = mkstemp(output_path);
  if (input_fd < 0 || output_fd < 0)
  {
    goto cleanup;
  }
  (void) close(output_fd);
  output_fd = -1;
  if (write_test_file(input_path, contents, content_length) != 0 ||
      unlink(output_path) != 0)
  {
    goto cleanup;
  }
  relay_fd = make_relay_socket(&relay_port);
  if (relay_fd < 0)
  {
    goto cleanup;
  }
  options.port = relay_port;
  relay_state.drop_first_data = inject_faults;
  relay_state.corrupt_first_data = inject_faults;
  relay_state.duplicate_data = inject_faults;
  relay_state.drop_fin_ack = inject_faults;
  relay_state.drop_all_fin_acks = drop_all_fin_acks;

  (void) fflush(NULL);
  receiver_pid = fork();
  if (receiver_pid < 0)
  {
    goto cleanup;
  }
  if (receiver_pid == 0)
  {
    exit(rdt_receive_file("127.0.0.1", relay_port, output_path, &options));
  }

  datagram_length = receive_relay_datagram(
      relay_fd, datagram, sizeof(datagram), &relay_state.receiver, 1000);
  if (datagram_length < 0 || datagram_length < 6 ||
      memcmp(datagram, "HELLO ", 6U) != 0)
  {
    goto cleanup;
  }
  relay_state.has_receiver = 1;
  if (send_relay_datagram(relay_fd, (const uint8_t *) "OK", 2U,
                          &relay_state.receiver) != 0)
  {
    goto cleanup;
  }

  sender_pid = fork();
  if (sender_pid < 0)
  {
    goto cleanup;
  }
  if (sender_pid == 0)
  {
    exit(rdt_send_file("127.0.0.1", relay_port, input_path, &options));
  }

  deadline = test_monotonic_milliseconds() + UINT64_C(8000);
  while (test_monotonic_milliseconds() < deadline &&
         (sender_pid > 0 || receiver_pid > 0))
  {
    struct sockaddr_in source;
    int remaining = 50;

    datagram_length = receive_relay_datagram(
        relay_fd, datagram, sizeof(datagram), &source, remaining);
    if (datagram_length >= 0)
    {
      handle_test_relay_packet(relay_fd, &relay_state, datagram,
                               (size_t) datagram_length, &source);
    }

    if (sender_pid > 0)
    {
      pid_t waited = waitpid(sender_pid, &sender_status, WNOHANG);
      if (waited == sender_pid)
      {
        sender_pid = -1;
      }
    }
    if (receiver_pid > 0)
    {
      pid_t waited = waitpid(receiver_pid, &receiver_status, WNOHANG);
      if (waited == receiver_pid)
      {
        receiver_pid = -1;
      }
    }
  }

  if (sender_pid > 0 || receiver_pid > 0)
  {
    goto cleanup;
  }
  if (!WIFEXITED(sender_status) ||
      WEXITSTATUS(sender_status) != (drop_all_fin_acks != 0 ? 2 : 0) ||
      !WIFEXITED(receiver_status) || WEXITSTATUS(receiver_status) != 0)
  {
    goto cleanup;
  }
  if (inject_faults != 0 &&
      (relay_state.drop_first_data != 0 ||
       relay_state.corrupt_first_data != 0 ||
       relay_state.duplicate_data != 0 || relay_state.drop_fin_ack != 0))
  {
    goto cleanup;
  }
  if (drop_all_fin_acks != 0 && relay_state.drop_all_fin_acks == 0)
  {
    goto cleanup;
  }

  received_file = fopen(output_path, "rb");
  if (received_file == NULL)
  {
    goto cleanup;
  }
  if (content_length > 0U)
  {
    received_contents = malloc(content_length);
    if (received_contents == NULL)
    {
      goto cleanup;
    }
    received_length = fread(received_contents, 1U, content_length,
                            received_file);
  }
  if (received_length != content_length ||
      (content_length > 0U &&
       memcmp(received_contents, contents, content_length) != 0))
  {
    goto cleanup;
  }
  result = 0;

cleanup:
  if (sender_pid > 0)
  {
    (void) kill(sender_pid, SIGTERM);
    finish_child(&sender_pid, &sender_status);
  }
  if (receiver_pid > 0)
  {
    (void) kill(receiver_pid, SIGTERM);
    finish_child(&receiver_pid, &receiver_status);
  }
  if (received_file != NULL)
  {
    (void) fclose(received_file);
  }
  free(received_contents);
  if (relay_fd >= 0)
  {
    (void) close(relay_fd);
  }
  if (input_fd >= 0)
  {
    (void) close(input_fd);
  }
  (void) unlink(input_path);
  (void) unlink(output_path);
  return result;
}

static void test_sender_receiver_with_faults(void)
{
  uint8_t contents[2500];
  size_t i;

  for (i = 0U; i < sizeof(contents); ++i)
  {
    contents[i] = (uint8_t) (i * 37U);
  }
  TEST_ASSERT_EQUAL_INT(0, run_transfer_case(contents, sizeof(contents),
                                             "test-rdt-faults", 1, 0));
}

static void test_empty_file_transfer(void)
{
  TEST_ASSERT_EQUAL_INT(0, run_transfer_case(NULL, 0U, "test-rdt-empty", 0,
                                             0));
}

static void test_sender_exits_two_after_fin_timeouts(void)
{
  TEST_ASSERT_EQUAL_INT(0, run_transfer_case(NULL, 0U, "test-rdt-fin-timeout",
                                             0, 1));
}

static void test_sender_gives_up_after_ten_data_timeouts(void)
{
  struct rdt_sender sender;
  struct rdt_protocol_actions actions;
  struct rdt_sender_event event = {0};
  uint8_t data[] = {0x41U};
  unsigned int timeout;

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, sizeof(data), 1U, 10U,
                                           0U, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_READ_FILE, actions.items[0].type);
  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.now_ms = 0U;
  event.bytes = data;
  event.length = sizeof(data);
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  for (timeout = 1U; timeout <= 10U; ++timeout)
  {
    event.type = RDT_SENDER_EVENT_TIMER;
    event.now_ms = (uint64_t) timeout * UINT64_C(10);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  }
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(2, actions.items[0].status);
}

static void test_sender_timeout_limit_resets_on_cumulative_progress(void)
{
  struct rdt_sender sender;
  struct rdt_protocol_actions actions;
  struct rdt_sender_event event = {0};
  uint8_t data[RDT_MAX_PAYLOAD] = {0};
  unsigned int timeout;

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, sizeof(data) * 2U, 1U,
                                           10U, 0U, &actions));
  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.bytes = data;
  event.length = sizeof(data);
  event.now_ms = 0U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  for (timeout = 1U; timeout <= 9U; ++timeout)
  {
    event.type = RDT_SENDER_EVENT_TIMER;
    event.now_ms = (uint64_t) timeout * UINT64_C(10);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  }

  event.type = RDT_SENDER_EVENT_ACK;
  event.now_ms = 90U;
  event.packet.type = RDT_ACK;
  event.packet.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_READ_FILE, actions.items[0].type);

  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.now_ms = 90U;
  event.bytes = data;
  event.length = sizeof(data);
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  for (timeout = 1U; timeout <= 9U; ++timeout)
  {
    event.type = RDT_SENDER_EVENT_TIMER;
    event.now_ms = UINT64_C(90) + (uint64_t) timeout * UINT64_C(10);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  }

  event.type = RDT_SENDER_EVENT_ACK;
  event.now_ms = 190U;
  event.packet.type = RDT_ACK;
  event.packet.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  event.packet.sequence = 3U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);
}

static void test_sender_window_cumulative_ack_and_timeout(void)
{
  struct rdt_sender sender;
  struct rdt_protocol_actions actions;
  struct rdt_sender_event event = {0};
  uint8_t payload[RDT_MAX_PAYLOAD] = {0};
  unsigned int sequence;
  enum rdt_packet_type type;
  uint32_t packet_sequence;

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, sizeof(payload) * 5U, 4U,
                                           10U, 0U, &actions));
  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.bytes = payload;
  event.length = sizeof(payload);
  for (sequence = 0U; sequence < 4U; ++sequence)
  {
    event.now_ms = (uint64_t) sequence;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
    TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  }
  TEST_ASSERT_EQUAL_UINT64(4U, sender.next_sequence);
  TEST_ASSERT_EQUAL_UINT64(0U, sender.base);
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[1].type);

  event.type = RDT_SENDER_EVENT_ACK;
  event.packet.type = RDT_ACK;
  event.packet.sequence = 0U;
  event.now_ms = 4U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_UINT64(0U, sender.base);
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);

  event.packet.sequence = 3U;
  event.now_ms = 5U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_UINT64(3U, sender.base);
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_READ_FILE, actions.items[0].type);

  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.now_ms = 6U;
  event.bytes = payload;
  event.length = sizeof(payload);
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  event.type = RDT_SENDER_EVENT_TIMER;
  event.now_ms = 16U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_UINT(3U, actions.count);
  for (sequence = 0U; sequence < 2U; ++sequence)
  {
    TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[sequence].type);
    TEST_ASSERT_EQUAL_INT(
        0, rdt_decode_packet(actions.items[sequence].bytes,
                             actions.items[sequence].length, &type,
                             &packet_sequence, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(sequence + 3U, packet_sequence);
  }
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[2].type);
}

static void test_sender_empty_and_exact_multiple_files(void)
{
  struct rdt_sender sender;
  struct rdt_protocol_actions actions;
  struct rdt_sender_event event = {0};
  uint8_t payload[RDT_MAX_PAYLOAD] = {0};

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 0U, 4U, 10U, 0U,
                                           &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[1].type);

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, sizeof(payload) * 2U,
                                           2U, 10U, 0U, &actions));
  event.type = RDT_SENDER_EVENT_DATA_READY;
  event.bytes = payload;
  event.length = sizeof(payload);
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  event.now_ms = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_UINT64(2U, sender.next_sequence);
  event.type = RDT_SENDER_EVENT_ACK;
  event.packet.type = RDT_ACK;
  event.packet.sequence = 2U;
  event.now_ms = 2U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(RDT_FIN, actions.items[0].bytes[0]);
}

static void test_protocol_state_machine_error_and_boundary_events(void)
{
  struct rdt_sender sender;
  struct rdt_receiver receiver;
  struct rdt_protocol_actions actions;
  struct rdt_sender_event sender_event = {0};
  struct rdt_receiver_event receiver_event = {0};
  uint8_t byte = 0x7fU;
  unsigned int timeout;

  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender, 1U, 0U, 10U, 0U,
                                            &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender, 1U, 65U, 10U, 0U,
                                            &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender, 1U, 1U, 0U, 0U,
                                            &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender, 1U, 1U, 60001U, 0U,
                                            &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender,
                                           (uint64_t) UINT32_MAX *
                                               RDT_MAX_PAYLOAD,
                                           1U, 10U, 0U, &actions));

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 1U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  sender_event.now_ms = 0U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  sender_event.type = RDT_SENDER_EVENT_ACK;
  sender_event.packet.type = RDT_ACK;
  sender_event.packet.sequence = 0U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_UINT(0U, actions.count);
  sender_event.type = RDT_SENDER_EVENT_DATA_READY;
  sender_event.now_ms = 0U;
  sender_event.bytes = &byte;
  sender_event.length = 0U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(1, actions.items[0].status);
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 1U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_DATA_READY;
  sender_event.bytes = &byte;
  sender_event.length = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  sender_event.now_ms = 5U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.type = RDT_SENDER_EVENT_ACK;
  sender_event.packet.type = RDT_DATA;
  sender_event.packet.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.packet.type = RDT_ACK;
  sender_event.packet.payload_length = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.packet.payload_length = 0U;
  sender_event.packet.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  sender_event.now_ms = 10U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);

  sender_event.type = RDT_SENDER_EVENT_ACK;
  sender_event.packet.type = RDT_ACK;
  sender_event.packet.sequence = 1U;
  sender_event.now_ms = 11U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  sender_event.packet.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 0U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  sender_event.now_ms = 5U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.type = RDT_SENDER_EVENT_ACK;
  sender_event.packet.type = RDT_DATA;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.packet.type = RDT_ACK;
  sender_event.packet.payload_length = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  sender_event.packet.payload_length = 0U;
  sender_event.packet.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  sender_event.now_ms = 10U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 0U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_TIMER;
  for (timeout = 1U; timeout <= 9U; ++timeout)
  {
    sender_event.now_ms = (uint64_t) timeout * UINT64_C(10);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
    TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
    TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[1].type);
  }
  sender_event.now_ms = 100U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(2, actions.items[0].status);

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 0U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_ACK;
  sender_event.packet.type = RDT_ACK;
  sender_event.packet.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);

  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_init(&receiver, 0U, &actions));
  receiver_event.type = RDT_RECEIVER_EVENT_FILE_OK;
  receiver_event.now_ms = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_UINT(0U, actions.count);
  receiver_event.type = RDT_RECEIVER_EVENT_PACKET;
  receiver_event.packet.type = RDT_DATA;
  receiver_event.packet.sequence = 1U;
  receiver_event.packet.payload_length = 1U;
  receiver_event.packet.payload[0] = byte;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  receiver_event.packet.sequence = 0U;
  receiver_event.packet.payload_length = 0U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  receiver_event.packet.payload_length = RDT_MAX_PAYLOAD + 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  receiver_event.packet.type = RDT_FIN;
  receiver_event.packet.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  receiver_event.packet.type = (enum rdt_packet_type) 99;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  receiver_event.packet.type = RDT_FIN;
  receiver_event.type = RDT_RECEIVER_EVENT_TIMER;
  receiver_event.now_ms = 5U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);

  receiver_event.type = RDT_RECEIVER_EVENT_IO_FAILURE;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(1, actions.items[0].status);
  receiver_event.type = RDT_RECEIVER_EVENT_PACKET;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &receiver_event,
                                             &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);

  TEST_ASSERT_NOT_EQUAL(
      0, rdt_sender_step(NULL, &sender_event, &actions));
  TEST_ASSERT_NOT_EQUAL(
      0, rdt_sender_step(&sender, NULL, &actions));
  TEST_ASSERT_NOT_EQUAL(
      0, rdt_sender_step(&sender, &sender_event, NULL));
  TEST_ASSERT_NOT_EQUAL(
      0, rdt_receiver_step(NULL, &receiver_event, &actions));
  TEST_ASSERT_NOT_EQUAL(
      0, rdt_receiver_step(&receiver, NULL, &actions));
  TEST_ASSERT_NOT_EQUAL(
      0, rdt_receiver_step(&receiver, &receiver_event, NULL));

  TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&sender, 1U, 1U, 10U, 0U,
                                           &actions));
  sender_event.type = RDT_SENDER_EVENT_DATA_READY;
  sender_event.bytes = NULL;
  sender_event.length = 1U;
  TEST_ASSERT_NOT_EQUAL(0,
      rdt_sender_step(&sender, &sender_event, &actions));
  sender_event.bytes = &byte;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_step(&sender, &sender_event, &actions));
  sender_event.type = RDT_SENDER_EVENT_IO_FAILURE;
  TEST_ASSERT_EQUAL_INT(0, rdt_sender_step(&sender, &sender_event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
}

#define MEMORY_CHANNEL_CAPACITY 128U

struct memory_channel
{
  uint8_t packets[MEMORY_CHANNEL_CAPACITY][RDT_MAX_PACKET];
  size_t lengths[MEMORY_CHANNEL_CAPACITY];
  size_t head;
  size_t count;
  uint32_t random_state;
  unsigned int dropped;
  unsigned int corrupted;
  unsigned int duplicated;
};

struct state_machine_transfer
{
  struct rdt_sender sender;
  struct rdt_receiver receiver;
  struct memory_channel sender_to_receiver;
  struct memory_channel receiver_to_sender;
  const uint8_t *source;
  size_t source_length;
  size_t source_offset;
  uint8_t destination[16384];
  size_t destination_length;
  uint64_t now_ms;
  int sender_done;
  int receiver_done;
  int status;
};

static uint32_t memory_channel_random(struct memory_channel *channel)
{
  uint32_t value = channel->random_state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  channel->random_state = value;
  return value;
}

static int memory_channel_enqueue(struct memory_channel *channel,
                                  const uint8_t *packet, size_t length)
{
  uint8_t impaired[RDT_MAX_PACKET];
  unsigned int copies;
  unsigned int copy;

  if (length > sizeof(impaired) ||
      memory_channel_random(channel) % 100U < 20U)
  {
    ++channel->dropped;
    return 0;
  }
  memcpy(impaired, packet, length);
  if (memory_channel_random(channel) % 100U < 20U)
  {
    impaired[length - 1U] ^= 1U;
    ++channel->corrupted;
  }
  copies = memory_channel_random(channel) % 100U < 20U ? 2U : 1U;
  if (copies == 2U)
  {
    ++channel->duplicated;
  }
  if (copies > MEMORY_CHANNEL_CAPACITY - channel->count)
  {
    return -1;
  }
  for (copy = 0U; copy < copies; ++copy)
  {
    size_t index = (channel->head + channel->count) %
                   MEMORY_CHANNEL_CAPACITY;
    memcpy(channel->packets[index], impaired, length);
    channel->lengths[index] = length;
    ++channel->count;
  }
  return 0;
}

static int memory_channel_pop(struct memory_channel *channel, uint8_t *packet,
                              size_t *length)
{
  if (channel->count == 0U)
  {
    return 0;
  }
  *length = channel->lengths[channel->head];
  memcpy(packet, channel->packets[channel->head], *length);
  channel->head = (channel->head + 1U) % MEMORY_CHANNEL_CAPACITY;
  --channel->count;
  return 1;
}

static int drive_sender_actions(
    struct state_machine_transfer *transfer,
    const struct rdt_protocol_actions *actions)
{
  size_t i;

  for (i = 0U; i < actions->count; ++i)
  {
    const struct rdt_protocol_action *action = &actions->items[i];
    struct rdt_protocol_actions next_actions;
    struct rdt_sender_event event = {0};

    if (action->type == RDT_ACTION_READ_FILE)
    {
      if (transfer->source_offset > transfer->source_length ||
          action->length > transfer->source_length -
                               transfer->source_offset)
      {
        return -1;
      }
      event.type = RDT_SENDER_EVENT_DATA_READY;
      event.now_ms = transfer->now_ms;
      event.bytes = transfer->source + transfer->source_offset;
      event.length = action->length;
      transfer->source_offset += action->length;
      if (rdt_sender_step(&transfer->sender, &event, &next_actions) != 0 ||
          drive_sender_actions(transfer, &next_actions) != 0)
      {
        return -1;
      }
      return 0;
    }
    if (action->type == RDT_ACTION_SEND_PACKET)
    {
      if (memory_channel_enqueue(&transfer->sender_to_receiver,
                                action->bytes, action->length) != 0)
      {
        return -1;
      }
    }
    else if (action->type == RDT_ACTION_COMPLETE)
    {
      transfer->sender_done = 1;
      transfer->status = action->status;
    }
    else if (action->type == RDT_ACTION_FAILED)
    {
      transfer->sender_done = 1;
      transfer->status = action->status;
    }
  }
  return 0;
}

static int drive_receiver_actions(
    struct state_machine_transfer *transfer,
    const struct rdt_protocol_actions *actions)
{
  size_t i;

  for (i = 0U; i < actions->count; ++i)
  {
    const struct rdt_protocol_action *action = &actions->items[i];
    struct rdt_protocol_actions next_actions;
    struct rdt_receiver_event event = {0};

    if (action->type == RDT_ACTION_WRITE_FILE)
    {
      if (action->length > sizeof(transfer->destination) -
                               transfer->destination_length)
      {
        return -1;
      }
      memcpy(transfer->destination + transfer->destination_length,
             action->bytes, action->length);
      transfer->destination_length += action->length;
      event.type = RDT_RECEIVER_EVENT_FILE_OK;
      event.now_ms = transfer->now_ms;
      if (rdt_receiver_step(&transfer->receiver, &event, &next_actions) != 0 ||
          drive_receiver_actions(transfer, &next_actions) != 0)
      {
        return -1;
      }
      return 0;
    }
    if (action->type == RDT_ACTION_FLUSH_FILE ||
        action->type == RDT_ACTION_CLOSE_FILE)
    {
      event.type = RDT_RECEIVER_EVENT_FILE_OK;
      event.now_ms = transfer->now_ms;
      if (rdt_receiver_step(&transfer->receiver, &event, &next_actions) != 0 ||
          drive_receiver_actions(transfer, &next_actions) != 0)
      {
        return -1;
      }
      return 0;
    }
    if (action->type == RDT_ACTION_SEND_PACKET)
    {
      if (memory_channel_enqueue(&transfer->receiver_to_sender,
                                action->bytes, action->length) != 0)
      {
        return -1;
      }
    }
    else if (action->type == RDT_ACTION_COMPLETE)
    {
      transfer->receiver_done = 1;
    }
    else if (action->type == RDT_ACTION_FAILED)
    {
      transfer->receiver_done = 1;
      transfer->status = action->status;
    }
  }
  return 0;
}

static int drive_sender_packet(struct state_machine_transfer *transfer,
                               const uint8_t *bytes, size_t length)
{
  struct rdt_sender_event event = {0};
  struct rdt_protocol_actions actions;
  const uint8_t *payload = NULL;

  if (rdt_decode_packet(bytes, length, &event.packet.type,
                        &event.packet.sequence, &payload,
                        &event.packet.payload_length) != 0)
  {
    return 0;
  }
  if (event.packet.type != RDT_ACK)
  {
    return 0;
  }
  event.type = RDT_SENDER_EVENT_ACK;
  event.now_ms = transfer->now_ms;
  (void) payload;
  if (rdt_sender_step(&transfer->sender, &event, &actions) != 0)
  {
    return -1;
  }
  return drive_sender_actions(transfer, &actions);
}

static int drive_receiver_packet(struct state_machine_transfer *transfer,
                                 const uint8_t *bytes, size_t length)
{
  struct rdt_receiver_event event = {0};
  struct rdt_protocol_actions actions;
  const uint8_t *payload = NULL;

  if (rdt_decode_packet(bytes, length, &event.packet.type,
                        &event.packet.sequence, &payload,
                        &event.packet.payload_length) != 0)
  {
    return 0;
  }
  if (event.packet.payload_length > 0U)
  {
    memcpy(event.packet.payload, payload, event.packet.payload_length);
  }
  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.now_ms = transfer->now_ms;
  if (rdt_receiver_step(&transfer->receiver, &event, &actions) != 0)
  {
    return -1;
  }
  return drive_receiver_actions(transfer, &actions);
}

static int run_seeded_state_machine_transfer(uint32_t seed,
                                            unsigned int impairment_counts[6])
{
  struct state_machine_transfer transfer = {0};
  struct rdt_protocol_actions actions;
  uint8_t packet[RDT_MAX_PACKET];
  size_t packet_length;
  size_t iteration;
  size_t i;
  struct rdt_sender_event sender_event = {0};
  struct rdt_receiver_event receiver_event = {0};
  const size_t source_length = 7U * RDT_MAX_PAYLOAD + 321U;
  uint8_t source[7U * RDT_MAX_PAYLOAD + 321U];

  transfer.source = source;
  transfer.source_length = source_length;
  transfer.sender_to_receiver.random_state = seed;
  transfer.receiver_to_sender.random_state = seed ^ UINT32_C(0x9e3779b9);
  for (i = 0U; i < source_length; ++i)
  {
    source[i] = (uint8_t) ((i * 73U + (size_t) seed) & 0xffU);
  }
  if (rdt_sender_init(&transfer.sender, source_length, 4U, 25U, 0U,
                      &actions) != 0 ||
      drive_sender_actions(&transfer, &actions) != 0 ||
      rdt_receiver_init(&transfer.receiver, 0U, &actions) != 0)
  {
    return -1;
  }

  for (iteration = 0U; iteration < 100000U; ++iteration)
  {
    int progressed = 0;

    if (transfer.sender_to_receiver.count > 0U &&
        memory_channel_pop(&transfer.sender_to_receiver, packet,
                           &packet_length) != 0)
    {
      if (drive_receiver_packet(&transfer, packet, packet_length) != 0)
      {
        fprintf(stderr, "seed=%u receiver packet action failed at %zu\n",
                seed, iteration);
        return -1;
      }
      progressed = 1;
    }
    if (transfer.receiver_to_sender.count > 0U &&
        memory_channel_pop(&transfer.receiver_to_sender, packet,
                           &packet_length) != 0)
    {
      if (drive_sender_packet(&transfer, packet, packet_length) != 0)
      {
        fprintf(stderr, "seed=%u sender packet action failed at %zu\n",
                seed, iteration);
        return -1;
      }
      progressed = 1;
    }
    if (transfer.sender_done != 0 && transfer.receiver_done != 0)
    {
      break;
    }
    if (progressed != 0)
    {
      continue;
    }

    {
      uint64_t next_deadline = UINT64_MAX;
      if (transfer.sender_done == 0 && transfer.sender.timer_active != 0 &&
          transfer.sender.deadline_ms < next_deadline)
      {
        next_deadline = transfer.sender.deadline_ms;
      }
      if (transfer.receiver_done == 0 &&
          transfer.receiver.phase == RDT_RECEIVER_ACTIVE &&
          transfer.receiver.idle_deadline_ms < next_deadline)
      {
        next_deadline = transfer.receiver.idle_deadline_ms;
      }
      if (transfer.receiver_done == 0 &&
          transfer.receiver.phase == RDT_RECEIVER_LINGER &&
          transfer.receiver.linger_deadline_ms < next_deadline)
      {
        next_deadline = transfer.receiver.linger_deadline_ms;
      }
      if (next_deadline == UINT64_MAX)
      {
        return -1;
      }
      transfer.now_ms = next_deadline;
    }

    if (transfer.sender_done == 0 && transfer.sender.timer_active != 0 &&
        transfer.sender.deadline_ms <= transfer.now_ms)
    {
      sender_event.type = RDT_SENDER_EVENT_TIMER;
      sender_event.now_ms = transfer.now_ms;
      if (rdt_sender_step(&transfer.sender, &sender_event, &actions) != 0 ||
          drive_sender_actions(&transfer, &actions) != 0)
      {
        fprintf(stderr, "seed=%u sender timer action failed at %zu\n", seed,
                iteration);
        return -1;
      }
    }
    if (transfer.receiver_done == 0 &&
        ((transfer.receiver.phase == RDT_RECEIVER_ACTIVE &&
          transfer.receiver.idle_deadline_ms <= transfer.now_ms) ||
         (transfer.receiver.phase == RDT_RECEIVER_LINGER &&
          transfer.receiver.linger_deadline_ms <= transfer.now_ms)))
    {
      receiver_event.type = RDT_RECEIVER_EVENT_TIMER;
      receiver_event.now_ms = transfer.now_ms;
      if (rdt_receiver_step(&transfer.receiver, &receiver_event, &actions) !=
              0 ||
          drive_receiver_actions(&transfer, &actions) != 0)
      {
        fprintf(stderr, "seed=%u receiver timer action failed at %zu\n", seed,
                iteration);
        return -1;
      }
    }
    if (transfer.status != 0)
    {
      return -1;
    }
  }

  if (iteration == 100000U || transfer.status != 0 ||
      transfer.sender_done == 0 || transfer.receiver_done == 0 ||
      transfer.source_offset != source_length ||
      transfer.destination_length != source_length ||
      memcmp(source, transfer.destination, source_length) != 0)
  {
    fprintf(stderr, "seed=%u iterations=%zu status=%d sender=%d receiver=%d "
                    "sent=%zu received=%zu queued=%zu/%zu time=%llu\n",
            seed, iteration, transfer.status, transfer.sender_done,
            transfer.receiver_done, transfer.source_offset,
            transfer.destination_length, transfer.sender_to_receiver.count,
            transfer.receiver_to_sender.count,
            (unsigned long long) transfer.now_ms);
    return -1;
  }
  impairment_counts[0] = transfer.sender_to_receiver.dropped;
  impairment_counts[1] = transfer.sender_to_receiver.corrupted;
  impairment_counts[2] = transfer.sender_to_receiver.duplicated;
  impairment_counts[3] = transfer.receiver_to_sender.dropped;
  impairment_counts[4] = transfer.receiver_to_sender.corrupted;
  impairment_counts[5] = transfer.receiver_to_sender.duplicated;
  return 0;
}

static void test_state_machines_transfer_with_seeded_impairments(void)
{
  static const uint32_t seeds[] = {
    UINT32_C(1), UINT32_C(0x12345678), UINT32_C(0xdeadbeef),
    UINT32_C(0x31415926), UINT32_C(0xabcdef01)
  };
  unsigned int impairment_totals[6] = {0U};
  size_t i;

  for (i = 0U; i < sizeof(seeds) / sizeof(seeds[0]); ++i)
  {
    unsigned int counts[6];
    size_t count_index;

    TEST_ASSERT_EQUAL_INT(
        0, run_seeded_state_machine_transfer(seeds[i], counts));
    for (count_index = 0U; count_index < 6U; ++count_index)
    {
      impairment_totals[count_index] += counts[count_index];
    }
  }
  for (i = 0U; i < 6U; ++i)
  {
    TEST_ASSERT_TRUE(impairment_totals[i] > 0U);
  }
}

static void test_send_file_failures(void)
{
  struct rdt_options options = {
    .session = "test-rdt-errors",
    .window_size = 8U,
    .timeout_ms = 50U,
    .port = 4250U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };

  TEST_ASSERT_NOT_EQUAL(0, rdt_send_file("127.0.0.1", options.port,
                                        "/no/such/cs425-file", &options));
  TEST_ASSERT_NOT_EQUAL(0, rdt_send_file("127.0.0.1", options.port,
                                        "/tmp", &options));
  TEST_ASSERT_NOT_EQUAL(0, rdt_send_file("invalid relay hostname.invalid",
                                        options.port, "/tmp", &options));
}

static void test_receive_output_open_failure(void)
{
  struct rdt_options options = {
    .session = "test-rdt-output",
    .window_size = 8U,
    .timeout_ms = 50U,
    .port = 0U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  struct sockaddr_in receiver;
  uint8_t datagram[128];
  unsigned short port = 0U;
  int relay_fd = make_relay_socket(&port);
  int datagram_length;
  pid_t receiver_pid;
  int status = 0;

  TEST_ASSERT_TRUE(relay_fd >= 0);
  options.port = port;
  (void) fflush(NULL);
  receiver_pid = fork();
  TEST_ASSERT_TRUE(receiver_pid >= 0);
  if (receiver_pid == 0)
  {
    exit(rdt_receive_file("127.0.0.1", port, "/no/such/directory/output",
                          &options));
  }
  datagram_length = receive_relay_datagram(
      relay_fd, datagram, sizeof(datagram), &receiver, 1000);
  TEST_ASSERT_TRUE(datagram_length >= 0);
  TEST_ASSERT_EQUAL_INT(0, send_relay_datagram(
                               relay_fd, (const uint8_t *) "OK", 2U,
                               &receiver));
  TEST_ASSERT_EQUAL_INT(receiver_pid, waitpid(receiver_pid, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_NOT_EQUAL(0, WEXITSTATUS(status));
  (void) close(relay_fd);
}

static void test_receive_transfer_failure_after_registration(void)
{
  struct rdt_options options = {
    .session = "test-rdt-full",
    .window_size = 1U,
    .timeout_ms = 50U,
    .port = 0U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  struct sockaddr_in receiver;
  uint8_t datagram[RDT_MAX_PACKET];
  const uint8_t payload[] = {'x'};
  unsigned short port = 0U;
  int relay_fd = make_relay_socket(&port);
  int datagram_length;
  size_t packet_length;
  pid_t receiver_pid;
  int status = 0;

  TEST_ASSERT_TRUE(relay_fd >= 0);
  options.port = port;
  (void) fflush(NULL);
  receiver_pid = fork();
  TEST_ASSERT_TRUE(receiver_pid >= 0);
  if (receiver_pid == 0)
  {
    exit(rdt_receive_file("127.0.0.1", port, "/dev/full", &options));
  }

  datagram_length = receive_relay_datagram(
      relay_fd, datagram, sizeof(datagram), &receiver, 1000);
  TEST_ASSERT_TRUE(datagram_length > 0);
  TEST_ASSERT_EQUAL_INT(
      0, send_relay_datagram(relay_fd, (const uint8_t *) "OK", 2U, &receiver));

  packet_length = rdt_encode_packet(datagram, sizeof(datagram), RDT_DATA, 0U,
                                    payload, sizeof(payload));
  TEST_ASSERT_TRUE(packet_length > 0U);
  TEST_ASSERT_EQUAL_INT(0, send_relay_datagram(
                               relay_fd, datagram, packet_length, &receiver));
  datagram_length = receive_relay_datagram(
      relay_fd, datagram, sizeof(datagram), &receiver, 1000);
  TEST_ASSERT_TRUE(datagram_length > 0);

  packet_length =
      rdt_encode_packet(datagram, sizeof(datagram), RDT_FIN, 1U, NULL, 0U);
  TEST_ASSERT_TRUE(packet_length > 0U);
  TEST_ASSERT_EQUAL_INT(0, send_relay_datagram(
                               relay_fd, datagram, packet_length, &receiver));
  TEST_ASSERT_EQUAL_INT(receiver_pid, waitpid(receiver_pid, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(1, WEXITSTATUS(status));
  (void) close(relay_fd);
}

static void test_receiver_linger_reacks_duplicate_fin_and_data(void)
{
  struct rdt_receiver receiver;
  struct rdt_protocol_actions actions;
  struct rdt_receiver_event event = {0};

  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_init(&receiver, 100U, &actions));
  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.now_ms = 100U;
  event.packet.type = RDT_DATA;
  event.packet.sequence = 0U;
  event.packet.payload_length = 1U;
  event.packet.payload[0] = 'x';
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WRITE_FILE, actions.items[0].type);
  TEST_ASSERT_EQUAL_UINT8('x', actions.items[0].bytes[0]);

  event.type = RDT_RECEIVER_EVENT_FILE_OK;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(RDT_ACK, actions.items[0].bytes[0]);
  TEST_ASSERT_EQUAL_INT(1U, actions.items[0].bytes[7]);

  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.packet.type = RDT_DATA;
  event.packet.sequence = 0U;
  event.now_ms = 101U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(1U, actions.items[0].bytes[7]);

  event.packet.sequence = 3U;
  event.now_ms = 102U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(1U, actions.items[0].bytes[7]);

  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.now_ms = 103U;
  event.packet.type = RDT_FIN;
  event.packet.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FLUSH_FILE, actions.items[0].type);
  event.type = RDT_RECEIVER_EVENT_FILE_OK;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_CLOSE_FILE, actions.items[0].type);
  event.now_ms = 102U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(2U, actions.items[0].bytes[7]);

  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.packet.type = RDT_FIN;
  event.packet.sequence = 1U;
  event.type = RDT_RECEIVER_EVENT_TIMER;
  event.now_ms = 200U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  event.type = RDT_RECEIVER_EVENT_PACKET;
  event.packet.type = RDT_DATA;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  event.packet.type = RDT_FIN;
  event.packet.sequence = 99U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_WAIT_UNTIL, actions.items[0].type);
  event.packet.sequence = 1U;
  event.now_ms = 201U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_SEND_PACKET, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(2U, actions.items[0].bytes[7]);
  event.type = RDT_RECEIVER_EVENT_TIMER;
  event.now_ms = 2102U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_COMPLETE, actions.items[0].type);
}

static void test_receiver_gives_up_after_30_seconds_without_valid_packet(void)
{
  struct rdt_receiver receiver;
  struct rdt_protocol_actions actions;
  struct rdt_receiver_event event = {0};

  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_init(&receiver, 500U, &actions));
  event.type = RDT_RECEIVER_EVENT_TIMER;
  event.now_ms = 30500U;
  TEST_ASSERT_EQUAL_INT(0, rdt_receiver_step(&receiver, &event, &actions));
  TEST_ASSERT_EQUAL_INT(RDT_ACTION_FAILED, actions.items[0].type);
  TEST_ASSERT_EQUAL_INT(2, actions.items[0].status);
}

static void test_null_and_invalid_pointer_arguments(void)
{
  struct rdt_options options = {
    .session = "null-check",
    .window_size = 8U,
    .timeout_ms = 50U,
    .port = 4250U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  struct rdt_sender sender;
  struct rdt_receiver receiver;
  struct rdt_protocol_actions actions;
  struct rdt_options null_session_options = options;
  uint8_t packet[RDT_MAX_PACKET];
  size_t packet_length;

  TEST_ASSERT_EQUAL_INT(0, rdt_io_close(NULL));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(NULL, 0U, 1U, 10U, 0U,
                                            &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_sender_init(&sender, 0U, 1U, 10U, 0U,
                                            NULL));
  TEST_ASSERT_NOT_EQUAL(0, rdt_receiver_init(NULL, 0U, &actions));
  TEST_ASSERT_NOT_EQUAL(0, rdt_receiver_init(&receiver, 0U, NULL));

  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(packet, sizeof(packet), RDT_DATA, 0U, NULL, 1U));
  TEST_ASSERT_EQUAL_UINT(
      0U, rdt_encode_packet(NULL, sizeof(packet), RDT_DATA, 0U, NULL, 0U));
  TEST_ASSERT_NOT_EQUAL(0, rdt_decode_packet(NULL, 0U, NULL, NULL, NULL, NULL));
  packet_length = rdt_encode_packet(packet, sizeof(packet), RDT_ACK, 9U,
                                    NULL, 0U);
  TEST_ASSERT_EQUAL_INT(0, rdt_decode_packet(packet, packet_length, NULL, NULL,
                                              NULL, NULL));
  TEST_ASSERT_NULL(rdt_io_open(NULL, options.port, "out", &options, 0, NULL));
  TEST_ASSERT_NULL(rdt_io_open("127.0.0.1", options.port, NULL, &options, 0,
                               NULL));
  TEST_ASSERT_NULL(rdt_io_open("127.0.0.1", options.port, "out", NULL, 0,
                               NULL));
  TEST_ASSERT_NULL(rdt_io_open("127.0.0.1", options.port, "out", &options, 2,
                               NULL));
  null_session_options.session = NULL;
  TEST_ASSERT_NULL(rdt_io_open("127.0.0.1", options.port, "out",
                               &null_session_options, 0, NULL));

  TEST_ASSERT_EQUAL_INT(1, rdt_send_file(NULL, options.port, "in", &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_send_file("127.0.0.1", options.port, NULL,
                                         &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_send_file("127.0.0.1", options.port, "in",
                                         NULL));
  TEST_ASSERT_EQUAL_INT(1, rdt_receive_file(NULL, options.port, "out",
                                            &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_receive_file("127.0.0.1", options.port, NULL,
                                            &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_receive_file("127.0.0.1", options.port, "out",
                                            NULL));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_send(NULL, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_receive(NULL));
}

static void test_io_dispatch_action_boundaries(void)
{
  TEST_ASSERT_EQUAL_INT(
      0, rdt_io_test_dispatch(RDT_ACTION_NONE, 0, 0, 1, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      0, rdt_io_test_dispatch(RDT_ACTION_NONE, 1, 0, 1, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_NONE, 0, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_NONE, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch((enum rdt_protocol_action_type) 99, 0, 0, 0, 0,
                              0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch((enum rdt_protocol_action_type) 99, 1, 0, 0, 0,
                              0));

  TEST_ASSERT_EQUAL_INT(
      0, rdt_io_test_dispatch(RDT_ACTION_COMPLETE, 0, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      0, rdt_io_test_dispatch(RDT_ACTION_COMPLETE, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_FAILED, 0, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      2, rdt_io_test_dispatch(RDT_ACTION_FAILED, 0, 2, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      2, rdt_io_test_dispatch(RDT_ACTION_FAILED, 0, 2, 0, 1, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_FAILED, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      2, rdt_io_test_dispatch(RDT_ACTION_FAILED, 1, 2, 0, 0, 0));

  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_WRITE_FILE, 0, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_FLUSH_FILE, 0, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_CLOSE_FILE, 0, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_READ_FILE, 1, 0, 0, 0, 0));

  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_READ_FILE, 0, 0, 0, 0, 1));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_SEND_PACKET, 0, 0, 0, 0, 1));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_WAIT_UNTIL, 0, 0, 0, 0, 1));

  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_WRITE_FILE, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_FLUSH_FILE, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_CLOSE_FILE, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_SEND_PACKET, 1, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT(
      1, rdt_io_test_dispatch(RDT_ACTION_WAIT_UNTIL, 1, 0, 0, 0, 1));
  TEST_ASSERT_EQUAL_INT(
      2, rdt_io_test_dispatch(RDT_ACTION_WAIT_UNTIL, 1, 0, 0, 0, 0));
}

static void test_transfer_status_mappings(void)
{
  TEST_ASSERT_EQUAL_INT(0, rdt_io_test_transfer_status(0, 0));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_test_transfer_status(1, 0));
  TEST_ASSERT_EQUAL_INT(2, rdt_io_test_transfer_status(2, 0));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_test_transfer_status(0, 1));
}

static void test_io_driver_context_paths(void)
{
  TEST_ASSERT_EQUAL_INT(1, rdt_io_test_drive(0, 1));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_test_drive(0, 0));
  TEST_ASSERT_EQUAL_INT(1, rdt_io_test_drive(1, 0));
}

static void run_relay_response_case(int sender_role, unsigned int window_size,
                                    const char *response, int expected_status)
{
  struct rdt_options options = {
    .session = sender_role != 0 ? "test-rdt-send-err" : "test-rdt-recv-err",
    .window_size = window_size,
    .timeout_ms = 50U,
    .port = 0U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  char input_path[] = "/tmp/cs425-rdt-hello-XXXXXX";
  uint8_t datagram[128];
  unsigned short port = 0U;
  int relay_fd = make_relay_socket(&port);
  int input_fd = -1;
  int datagram_length;
  int status = 0;
  unsigned int attempt;
  pid_t worker;
  struct sockaddr_in peer;

  TEST_ASSERT_TRUE(relay_fd >= 0);
  options.port = port;
  if (sender_role != 0)
  {
    input_fd = mkstemp(input_path);
    TEST_ASSERT_TRUE(input_fd >= 0);
    (void) close(input_fd);
  }

  (void) fflush(NULL);
  worker = fork();
  TEST_ASSERT_TRUE(worker >= 0);
  if (worker == 0)
  {
    if (sender_role == 2)
    {
      struct rdt_io_context *io =
          rdt_io_open("127.0.0.1", port, input_path, &options, 1, NULL);
      if (io == NULL)
      {
        exit(1);
      }
      exit(rdt_io_close(io) == 0 ? 0 : 1);
    }
    if (sender_role != 0)
    {
      exit(rdt_send_file("127.0.0.1", port, input_path, &options));
    }
    exit(rdt_receive_file("127.0.0.1", port, "/tmp/unused-rdt-output",
                          &options));
  }

  for (attempt = 0U; attempt < (response == NULL ? 5U : 1U); ++attempt)
  {
    datagram_length = receive_relay_datagram(relay_fd, datagram,
                                             sizeof(datagram), &peer, 2000);
    TEST_ASSERT_TRUE(datagram_length > 0);
    if (response != NULL)
    {
      TEST_ASSERT_EQUAL_INT(0, send_relay_datagram(
                                   relay_fd, (const uint8_t *) response,
                                   strlen(response), &peer));
    }
  }
  TEST_ASSERT_EQUAL_INT(worker, waitpid(worker, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(expected_status, WEXITSTATUS(status));
  if (input_fd >= 0)
  {
    (void) unlink(input_path);
  }
  (void) close(relay_fd);
}

static void test_sender_relay_rejection(void)
{
  run_relay_response_case(1, 8U, "ERR intentional test error", 1);
}

static void test_receiver_relay_rejection(void)
{
  run_relay_response_case(0, 8U, "ERR intentional test error", 1);
}

static void test_receiver_rejects_unexpected_hello_response(void)
{
  run_relay_response_case(0, 8U, "NOPE", 1);
}

static void test_receiver_rejects_two_byte_hello_response(void)
{
  run_relay_response_case(0, 8U, "NO", 1);
}

static void test_sender_rejects_invalid_protocol_configuration(void)
{
  run_relay_response_case(1, 0U, "OK", 1);
}

static void test_sender_open_without_size_output(void)
{
  run_relay_response_case(2, 8U, "OK", 0);
}

static void test_relay_handshake_retries_without_response(void)
{
  run_relay_response_case(0, 8U, NULL, 1);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_session_validation);
  RUN_TEST(test_packet_round_trip);
  RUN_TEST(test_rfc1071_checksum_vectors);
  RUN_TEST(test_packet_rejects_corruption_and_bad_lengths);
  RUN_TEST(test_packet_odd_payload_and_single_bit_checksum_detection);
  RUN_TEST(test_ack_and_fin_type_values);
  RUN_TEST(test_encode_rejects_oversized_or_missing_payload);
  RUN_TEST(test_sender_receiver_with_faults);
  RUN_TEST(test_empty_file_transfer);
  RUN_TEST(test_sender_exits_two_after_fin_timeouts);
  RUN_TEST(test_sender_gives_up_after_ten_data_timeouts);
  RUN_TEST(test_sender_timeout_limit_resets_on_cumulative_progress);
  RUN_TEST(test_sender_window_cumulative_ack_and_timeout);
  RUN_TEST(test_sender_empty_and_exact_multiple_files);
  RUN_TEST(test_protocol_state_machine_error_and_boundary_events);
  RUN_TEST(test_state_machines_transfer_with_seeded_impairments);
  RUN_TEST(test_send_file_failures);
  RUN_TEST(test_receive_output_open_failure);
  RUN_TEST(test_receive_transfer_failure_after_registration);
  RUN_TEST(test_receiver_linger_reacks_duplicate_fin_and_data);
  RUN_TEST(test_receiver_gives_up_after_30_seconds_without_valid_packet);
  RUN_TEST(test_null_and_invalid_pointer_arguments);
  RUN_TEST(test_sender_relay_rejection);
  RUN_TEST(test_receiver_relay_rejection);
  RUN_TEST(test_receiver_rejects_unexpected_hello_response);
  RUN_TEST(test_receiver_rejects_two_byte_hello_response);
  RUN_TEST(test_sender_rejects_invalid_protocol_configuration);
  RUN_TEST(test_sender_open_without_size_output);
  RUN_TEST(test_relay_handshake_retries_without_response);
  RUN_TEST(test_io_dispatch_action_boundaries);
  RUN_TEST(test_transfer_status_mappings);
  RUN_TEST(test_io_driver_context_paths);
  return UNITY_END();
}

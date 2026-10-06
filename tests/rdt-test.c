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

static int null_test_send_datagram(void *context, const uint8_t *packet,
                                   size_t length)
{
  (void) context;
  (void) packet;
  (void) length;
  return 0;
}

static int null_test_receive_datagram(void *context, uint8_t *packet,
                                      size_t capacity, size_t *length,
                                      int timeout_ms)
{
  (void) context;
  (void) packet;
  (void) capacity;
  (void) length;
  (void) timeout_ms;
  return -1;
}

static uint64_t null_test_now_ms(void *context)
{
  (void) context;
  return 0U;
}

static int null_test_read_file(void *context, uint8_t *bytes, size_t length)
{
  (void) context;
  (void) bytes;
  (void) length;
  return 0;
}

static int null_test_write_file(void *context, const uint8_t *bytes,
                                size_t length)
{
  (void) context;
  (void) bytes;
  (void) length;
  return 0;
}

static int null_test_flush_file(void *context)
{
  (void) context;
  return 0;
}

struct linger_test_context
{
  uint64_t now_ms;
  uint8_t incoming[4][RDT_MAX_PACKET];
  size_t incoming_lengths[4];
  size_t incoming_count;
  size_t next_incoming;
  uint32_t acknowledgements[4];
  size_t acknowledgement_count;
  uint8_t written[8];
  size_t written_length;
  unsigned int flush_count;
};

static int linger_test_send(void *context, const uint8_t *packet,
                            size_t length)
{
  struct linger_test_context *test = context;
  enum rdt_packet_type type;
  uint32_t sequence;

  if (test->acknowledgement_count >=
          sizeof(test->acknowledgements) / sizeof(test->acknowledgements[0]) ||
      rdt_decode_packet(packet, length, &type, &sequence, NULL, NULL) != 0 ||
      type != RDT_ACK)
  {
    return -1;
  }
  test->acknowledgements[test->acknowledgement_count] = sequence;
  ++test->acknowledgement_count;
  return 0;
}

static int linger_test_receive(void *context, uint8_t *packet,
                               size_t capacity, size_t *length,
                               int timeout_ms)
{
  struct linger_test_context *test = context;

  if (test->next_incoming < test->incoming_count)
  {
    size_t packet_length = test->incoming_lengths[test->next_incoming];
    if (capacity < packet_length)
    {
      return -1;
    }
    memcpy(packet, test->incoming[test->next_incoming], packet_length);
    *length = packet_length;
    ++test->next_incoming;
    return 0;
  }
  if (timeout_ms < 0)
  {
    return -1;
  }
  test->now_ms += (uint64_t) timeout_ms;
  return 1;
}

static uint64_t linger_test_clock(void *context)
{
  const struct linger_test_context *test = context;
  return test->now_ms;
}

static int linger_test_read(void *context, uint8_t *bytes, size_t length)
{
  (void) context;
  (void) bytes;
  (void) length;
  return -1;
}

static int linger_test_write(void *context, const uint8_t *bytes,
                             size_t length)
{
  struct linger_test_context *test = context;

  if (length > sizeof(test->written) - test->written_length)
  {
    return -1;
  }
  memcpy(test->written + test->written_length, bytes, length);
  test->written_length += length;
  return 0;
}

static int linger_test_flush(void *context)
{
  struct linger_test_context *test = context;
  ++test->flush_count;
  return 0;
}

static int queue_linger_packet(struct linger_test_context *test,
                               enum rdt_packet_type type, uint32_t sequence,
                               const uint8_t *payload, uint32_t length)
{
  size_t packet_length;

  if (test->incoming_count >=
      sizeof(test->incoming) / sizeof(test->incoming[0]))
  {
    return -1;
  }
  packet_length = rdt_encode_packet(test->incoming[test->incoming_count],
                                    sizeof(test->incoming[0]), type, sequence,
                                    payload, length);
  if (packet_length == 0U)
  {
    return -1;
  }
  test->incoming_lengths[test->incoming_count] = packet_length;
  ++test->incoming_count;
  return 0;
}

static void test_receiver_linger_reacks_duplicate_fin_and_data(void)
{
  const uint8_t data[] = {'x'};
  uint8_t fin_timeout[] = {0U, 50U};
  struct linger_test_context test = {.now_ms = 100U};
  struct rdt_protocol_io io = {
    .context = &test,
    .send_datagram = linger_test_send,
    .receive_datagram = linger_test_receive,
    .now_ms = linger_test_clock,
    .read_file = linger_test_read,
    .write_file = linger_test_write,
    .flush_file = linger_test_flush
  };
  struct rdt_options options = {
    .session = "linger-test",
    .window_size = 4U,
    .timeout_ms = 50U,
    .port = 4250U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  uint64_t start_ms = test.now_ms;

  TEST_ASSERT_EQUAL_INT(0, queue_linger_packet(&test, RDT_DATA, 0U, data,
                                                sizeof(data)));
  TEST_ASSERT_EQUAL_INT(0, queue_linger_packet(&test, RDT_FIN, 1U,
                                                fin_timeout,
                                                sizeof(fin_timeout)));
  TEST_ASSERT_EQUAL_INT(0, queue_linger_packet(&test, RDT_DATA, 0U, data,
                                                sizeof(data)));
  TEST_ASSERT_EQUAL_INT(0, queue_linger_packet(&test, RDT_FIN, 1U,
                                                fin_timeout,
                                                sizeof(fin_timeout)));

  TEST_ASSERT_EQUAL_INT(0, rdt_protocol_receive(&io, &options));
  TEST_ASSERT_EQUAL_UINT(1U, test.written_length);
  TEST_ASSERT_EQUAL_UINT8('x', test.written[0]);
  TEST_ASSERT_EQUAL_UINT(1U, test.flush_count);
  TEST_ASSERT_EQUAL_UINT(4U, test.acknowledgement_count);
  TEST_ASSERT_EQUAL_UINT32(1U, test.acknowledgements[0]);
  TEST_ASSERT_EQUAL_UINT32(2U, test.acknowledgements[1]);
  TEST_ASSERT_EQUAL_UINT32(1U, test.acknowledgements[2]);
  TEST_ASSERT_EQUAL_UINT32(2U, test.acknowledgements[3]);
  TEST_ASSERT_EQUAL_UINT64(50U * 10U + 1000U, test.now_ms - start_ms);
}

static void test_receiver_gives_up_after_30_seconds_without_valid_packet(void)
{
  struct linger_test_context test = {.now_ms = 500U};
  struct rdt_protocol_io io = {
    .context = &test,
    .send_datagram = linger_test_send,
    .receive_datagram = linger_test_receive,
    .now_ms = linger_test_clock,
    .read_file = linger_test_read,
    .write_file = linger_test_write,
    .flush_file = linger_test_flush
  };
  struct rdt_options options = {
    .session = "idle-timeout",
    .window_size = 4U,
    .timeout_ms = 50U,
    .port = 4250U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  uint64_t start_ms = test.now_ms;

  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&io, &options));
  TEST_ASSERT_EQUAL_UINT64(UINT64_C(30000), test.now_ms - start_ms);
  TEST_ASSERT_EQUAL_UINT(0U, test.written_length);
  TEST_ASSERT_EQUAL_UINT(0U, test.acknowledgement_count);
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
  struct rdt_protocol_io protocol_io = {0};
  struct rdt_protocol_io complete_io = {
    .context = NULL,
    .send_datagram = null_test_send_datagram,
    .receive_datagram = null_test_receive_datagram,
    .now_ms = null_test_now_ms,
    .read_file = null_test_read_file,
    .write_file = null_test_write_file,
    .flush_file = null_test_flush_file
  };
  struct rdt_options null_session_options = options;
  uint8_t packet[RDT_MAX_PACKET];
  size_t packet_length;

  TEST_ASSERT_EQUAL_INT(0, rdt_io_close(NULL));
  rdt_io_get_protocol_io(NULL, NULL);
  rdt_io_get_protocol_io(NULL, &protocol_io);
  TEST_ASSERT_NULL(protocol_io.context);
  TEST_ASSERT_NULL(protocol_io.send_datagram);
  TEST_ASSERT_NULL(protocol_io.receive_datagram);
  TEST_ASSERT_NULL(protocol_io.now_ms);
  TEST_ASSERT_NULL(protocol_io.read_file);
  TEST_ASSERT_NULL(protocol_io.write_file);
  TEST_ASSERT_NULL(protocol_io.flush_file);

  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(NULL, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, NULL));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(NULL, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, NULL));

  complete_io.send_datagram = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));
  complete_io.send_datagram = null_test_send_datagram;
  complete_io.receive_datagram = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));
  complete_io.receive_datagram = null_test_receive_datagram;
  complete_io.now_ms = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));
  complete_io.now_ms = null_test_now_ms;
  complete_io.read_file = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));
  complete_io.read_file = null_test_read_file;
  complete_io.write_file = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));
  complete_io.write_file = null_test_write_file;
  complete_io.flush_file = NULL;
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_send(&complete_io, 0U, &options));
  TEST_ASSERT_EQUAL_INT(1, rdt_protocol_receive(&complete_io, &options));

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
}

static void run_relay_rejection_case(int sender_role)
{
  struct rdt_options options = {
    .session = sender_role != 0 ? "test-rdt-send-err" : "test-rdt-recv-err",
    .window_size = 8U,
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
    if (sender_role != 0)
    {
      exit(rdt_send_file("127.0.0.1", port, input_path, &options));
    }
    exit(rdt_receive_file("127.0.0.1", port, "/tmp/unused-rdt-output",
                          &options));
  }

  datagram_length = receive_relay_datagram(relay_fd, datagram,
                                           sizeof(datagram), &peer, 1000);
  TEST_ASSERT_TRUE(datagram_length > 0);
  TEST_ASSERT_EQUAL_INT(0, send_relay_datagram(
                               relay_fd,
                               (const uint8_t *) "ERR intentional test error",
                               strlen("ERR intentional test error"), &peer));
  TEST_ASSERT_EQUAL_INT(worker, waitpid(worker, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_NOT_EQUAL(0, WEXITSTATUS(status));
  if (input_fd >= 0)
  {
    (void) unlink(input_path);
  }
  (void) close(relay_fd);
}

static void test_sender_relay_rejection(void)
{
  run_relay_rejection_case(1);
}

static void test_receiver_relay_rejection(void)
{
  run_relay_rejection_case(0);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_session_validation);
  RUN_TEST(test_packet_round_trip);
  RUN_TEST(test_packet_rejects_corruption_and_bad_lengths);
  RUN_TEST(test_ack_and_fin_type_values);
  RUN_TEST(test_encode_rejects_oversized_or_missing_payload);
  RUN_TEST(test_sender_receiver_with_faults);
  RUN_TEST(test_empty_file_transfer);
  RUN_TEST(test_sender_exits_two_after_fin_timeouts);
  RUN_TEST(test_send_file_failures);
  RUN_TEST(test_receive_output_open_failure);
  RUN_TEST(test_receiver_linger_reacks_duplicate_fin_and_data);
  RUN_TEST(test_receiver_gives_up_after_30_seconds_without_valid_packet);
  RUN_TEST(test_null_and_invalid_pointer_arguments);
  RUN_TEST(test_sender_relay_rejection);
  RUN_TEST(test_receiver_relay_rejection);
  return UNITY_END();
}

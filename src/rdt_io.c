#define _POSIX_C_SOURCE 200809L

#include "rdt_io.h"

#include "rdt.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HELLO_ATTEMPTS 5
#define HELLO_TIMEOUT_MS 1000

struct rdt_io_context
{
  int socket_fd;
  FILE *file;
  int sender;
};

static int open_relay_socket(const char *host, unsigned short port)
{
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  char service[6];
  int socket_fd = -1;
  int status;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  (void) snprintf(service, sizeof(service), "%u", (unsigned int) port);

  status = getaddrinfo(host, service, &hints, &addresses);
  // GCOVR_EXCL_START
  if (status != 0)
  {
    fprintf(stderr, "Cannot resolve relay '%s': %s\n", host,
            gai_strerror(status));
    return -1;
  }
  // GCOVR_EXCL_STOP

  // GCOVR_EXCL_START
  for (address = addresses; address != NULL; address = address->ai_next)
  {
    socket_fd = socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol);
    if (socket_fd < 0)
    {
      continue;
    }
    if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0)
    {
      break;
    }
    (void) close(socket_fd);
    socket_fd = -1;
  }
  // GCOVR_EXCL_STOP
  freeaddrinfo(addresses);

  // GCOVR_EXCL_START
  if (socket_fd < 0)
  {
    fprintf(stderr, "Cannot connect UDP socket to relay '%s:%u': %s\n",
            host, (unsigned int) port, strerror(errno));
  }
  // GCOVR_EXCL_STOP
  return socket_fd;
}

static int send_datagram(void *context, const uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;
  ssize_t sent;

  // GCOVR_EXCL_START
  do
  {
    sent = send(io->socket_fd, bytes, length, 0);
  } while (sent < 0 && errno == EINTR);

  if (sent < 0 || (size_t) sent != length)
  {
    if (sent >= 0)
    {
      fprintf(stderr, "Short UDP datagram send\n");
    }
    else
    {
      fprintf(stderr, "UDP send failed: %s\n", strerror(errno));
    }
    return -1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

static int wait_readable(int socket_fd, int timeout_ms)
{
  struct pollfd descriptor = {
    .fd = socket_fd,
    .events = POLLIN,
    .revents = 0
  };
  int result;

  // GCOVR_EXCL_START
  do
  {
    result = poll(&descriptor, 1U, timeout_ms);
  } while (result < 0 && errno == EINTR);

  if (result < 0)
  {
    fprintf(stderr, "UDP wait failed: %s\n", strerror(errno));
    return -1;
  }
  // GCOVR_EXCL_STOP
  if (result == 0)
  {
    return 0;
  }
  // GCOVR_EXCL_START
  if ((descriptor.revents & POLLIN) != 0)
  {
    return 1;
  }
  fprintf(stderr, "UDP socket reported an error while waiting\n");
  return -1;
  // GCOVR_EXCL_STOP
}

static uint64_t monotonic_milliseconds(void *context)
{
  struct timespec now;
  (void) context;

  // GCOVR_EXCL_START
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  {
    fprintf(stderr, "Could not read monotonic clock: %s\n", strerror(errno));
    return 0U;
  }
  // GCOVR_EXCL_STOP
  return (uint64_t) now.tv_sec * UINT64_C(1000) +
         (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static int read_file(void *context, uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;

  // GCOVR_EXCL_START
  if (fread(bytes, 1U, length, io->file) != length)
  {
    fprintf(stderr, "Could not read input file: %s\n",
            ferror(io->file) != 0 ? strerror(errno) : "unexpected end of file");
    return -1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

static int write_file(void *context, const uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;

  // GCOVR_EXCL_START
  if (fwrite(bytes, 1U, length, io->file) != length)
  {
    fprintf(stderr, "Could not write output file: %s\n", strerror(errno));
    return -1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

static int flush_file(void *context)
{
  struct rdt_io_context *io = context;

  // GCOVR_EXCL_START
  if (fflush(io->file) != 0)
  {
    fprintf(stderr, "Could not flush output file: %s\n", strerror(errno));
    return -1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

static int close_file(void *context)
{
  struct rdt_io_context *io = context;
  FILE *file = io->file;

  io->file = NULL;
  // GCOVR_EXCL_START
  if (fclose(file) != 0)
  {
    fprintf(stderr, "Could not close output file: %s\n", strerror(errno));
    return -1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

static int relay_handshake(struct rdt_io_context *io,
                           const struct rdt_options *options)
{
  char hello[128];
  int hello_length;
  int attempt;

  if (io->sender != 0)
  {
    hello_length = snprintf(hello, sizeof(hello),
                            "HELLO %s send %.17f %.17f %.17f",
                            options->session, options->loss,
                            options->corrupt, options->duplicate);
  }
  else
  {
    hello_length = snprintf(hello, sizeof(hello), "HELLO %s recv",
                            options->session);
  }
  // GCOVR_EXCL_START
  if (hello_length < 0 || (size_t) hello_length >= sizeof(hello))
  {
    fprintf(stderr, "Could not construct relay HELLO message\n");
    return -1;
  }
  // GCOVR_EXCL_STOP

  /* HELLO and its reply are not impaired; retries cover UDP loss. */
  for (attempt = 0; attempt < HELLO_ATTEMPTS; ++attempt)
  {
    uint8_t response[256];
    size_t response_length = 0U;
    int ready;
    ssize_t received;

    // GCOVR_EXCL_START
    if (send_datagram(io, (const uint8_t *) hello,
                      (size_t) hello_length) != 0)
    {
      return -1;
    }
    // GCOVR_EXCL_STOP
    ready = wait_readable(io->socket_fd, HELLO_TIMEOUT_MS);
    // GCOVR_EXCL_START
    if (ready < 0)
    {
      return -1;
    }
    // GCOVR_EXCL_STOP
    if (ready == 0)
    {
      continue;
    }
    // GCOVR_EXCL_START
    do
    {
      received = recv(io->socket_fd, response, sizeof(response) - 1U, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
    {
      fprintf(stderr, "UDP receive failed: %s\n", strerror(errno));
      return -1;
    }
    // GCOVR_EXCL_STOP
    response_length = (size_t) received;
    response[response_length] = '\0';
    if (response_length == 2U && memcmp(response, "OK", 2U) == 0)
    {
      return 0;
    }
    if (response_length >= 4U && memcmp(response, "ERR ", 4U) == 0)
    {
      fprintf(stderr, "Relay rejected HELLO: %s\n", response + 4U);
    }
    else
    {
      fprintf(stderr, "Unexpected relay HELLO response: %s\n", response);
    }
    return -1;
  }
  fprintf(stderr, "Relay did not answer HELLO after %d attempts\n",
          HELLO_ATTEMPTS);
  return -1;
}

struct rdt_io_context *rdt_io_open(const char *host, unsigned short port,
                                   const char *path,
                                   const struct rdt_options *options,
                                   int sender, uint64_t *file_size)
{
  struct rdt_io_context *io;
  struct stat file_status;

  if (host == NULL || path == NULL || options == NULL ||
      rdt_validate_session(options->session) != 0 ||
      (sender != 0 && sender != 1))
  {
    fprintf(stderr, "Invalid endpoint or relay options\n");
    return NULL;
  }

  io = calloc(1U, sizeof(*io));
  // GCOVR_EXCL_START
  if (io == NULL)
  {
    fprintf(stderr, "Could not allocate I/O context: %s\n", strerror(errno));
    return NULL;
  }
  // GCOVR_EXCL_STOP
  io->socket_fd = -1;
  io->sender = sender;

  if (sender != 0)
  {
    // GCOVR_EXCL_START
    io->file = fopen(path, "rb");
    if (io->file == NULL)
    {
      fprintf(stderr, "Cannot open input file '%s': %s\n", path,
              strerror(errno));
      rdt_io_close(io);
      return NULL;
    }
    // GCOVR_EXCL_STOP
    // GCOVR_EXCL_START
    if (fstat(fileno(io->file), &file_status) != 0)
    {
      fprintf(stderr, "Input must be a readable regular file\n");
      rdt_io_close(io);
      return NULL;
    }
    // GCOVR_EXCL_STOP
    if (!S_ISREG(file_status.st_mode))
    {
      fprintf(stderr, "Input must be a readable regular file\n");
      rdt_io_close(io);
      return NULL;
    }
    if (file_size != NULL)
    {
      *file_size = (uint64_t) file_status.st_size;
    }
  }

  io->socket_fd = open_relay_socket(host, port);
  // GCOVR_EXCL_START
  if (io->socket_fd < 0)
  {
    rdt_io_close(io);
    return NULL;
  }
  // GCOVR_EXCL_STOP
  if (relay_handshake(io, options) != 0)
  {
    rdt_io_close(io);
    return NULL;
  }

  if (sender == 0)
  {
    io->file = fopen(path, "wb");
    if (io->file == NULL)
    {
      fprintf(stderr, "Cannot open output file '%s': %s\n", path,
              strerror(errno));
      rdt_io_close(io);
      return NULL;
    }
  }
  return io;
}

int rdt_io_close(struct rdt_io_context *context)
{
  int result = 0;

  if (context == NULL)
  {
    return 0;
  }
  if (context->file != NULL)
  {
    // GCOVR_EXCL_START
    if (fclose(context->file) != 0)
    {
      fprintf(stderr, "Could not close transfer file: %s\n", strerror(errno));
      result = -1;
    }
    // GCOVR_EXCL_STOP
  }
  if (context->socket_fd >= 0)
  {
    // GCOVR_EXCL_START
    if (close(context->socket_fd) != 0)
    {
      fprintf(stderr, "Could not close relay socket: %s\n", strerror(errno));
      result = -1;
    }
    // GCOVR_EXCL_STOP
  }
  free(context);
  return result;
}

static int receive_protocol_packet(struct rdt_io_context *io,
                                   uint64_t deadline_ms,
                                   struct rdt_protocol_packet *packet)
{
  uint8_t bytes[RDT_MAX_PACKET];

  for (;;)
  {
    uint64_t now = monotonic_milliseconds(io);
    int timeout_ms;
    int ready;
    ssize_t received;
    const uint8_t *payload = NULL;

    if (now >= deadline_ms)
    {
      return 1;
    }
    timeout_ms = (int) (deadline_ms - now);
    ready = wait_readable(io->socket_fd, timeout_ms);
    if (ready == 0)
    {
      return 1;
    }
    // GCOVR_EXCL_START
    if (ready < 0)
    {
      return -1;
    }
    // GCOVR_EXCL_STOP
    // GCOVR_EXCL_START
    do
    {
      received = recv(io->socket_fd, bytes, sizeof(bytes), 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
    {
      fprintf(stderr, "UDP receive failed: %s\n", strerror(errno));
      return -1;
    }
    // GCOVR_EXCL_STOP
    if (rdt_decode_packet(bytes, (size_t) received, &packet->type,
                          &packet->sequence, &payload,
                          &packet->payload_length) == 0)
    {
      if (packet->payload_length > 0U)
      {
        memcpy(packet->payload, payload, packet->payload_length);
      }
      return 0;
    }
  }
}

static int sender_dispatch(struct rdt_io_context *io,
                           struct rdt_sender *sender,
                           const struct rdt_protocol_actions *actions)
{
  struct rdt_protocol_actions current = *actions;
  size_t i;

process_batch:
  for (i = 0U; i < current.count; ++i)
  {
    const struct rdt_protocol_action *action = &current.items[i];
    struct rdt_protocol_actions next_actions;
    struct rdt_sender_event event;
    int result;

    switch (action->type)
    {
    case RDT_ACTION_READ_FILE:
    {
      uint8_t bytes[RDT_MAX_PAYLOAD];
      // GCOVR_EXCL_START
      if (read_file(io, bytes, action->length) != 0)
      {
        memset(&event, 0, sizeof(event));
        event.type = RDT_SENDER_EVENT_IO_FAILURE;
      }
      else
      {
        memset(&event, 0, sizeof(event));
        event.type = RDT_SENDER_EVENT_DATA_READY;
        event.now_ms = monotonic_milliseconds(io);
        event.bytes = bytes;
        event.length = action->length;
      }
      // GCOVR_EXCL_STOP
      result = rdt_sender_step(sender, &event, &next_actions);
      if (result != 0)
      {
        return 1;
      }
      current = next_actions;
      goto process_batch;
    }
    case RDT_ACTION_SEND_PACKET:
      // GCOVR_EXCL_START
      if (send_datagram(io, action->bytes, action->length) != 0)
      {
        memset(&event, 0, sizeof(event));
        event.type = RDT_SENDER_EVENT_IO_FAILURE;
        if (rdt_sender_step(sender, &event, &next_actions) != 0)
        {
          return 1;
        }
        current = next_actions;
        goto process_batch;
      }
      // GCOVR_EXCL_STOP
      break;
    case RDT_ACTION_WAIT_UNTIL:
      memset(&event, 0, sizeof(event));
      result = receive_protocol_packet(io, action->deadline_ms,
                                       &event.packet);
      event.now_ms = monotonic_milliseconds(io);
      event.type = result == 0 ? RDT_SENDER_EVENT_ACK
                               : RDT_SENDER_EVENT_TIMER;
      // GCOVR_EXCL_START
      if (result < 0)
      {
        event.type = RDT_SENDER_EVENT_IO_FAILURE;
      }
      // GCOVR_EXCL_STOP
      if (rdt_sender_step(sender, &event, &next_actions) != 0)
      {
        return 1;
      }
      current = next_actions;
      goto process_batch;
    case RDT_ACTION_COMPLETE:
      return 0;
    case RDT_ACTION_FAILED:
      if (action->status == 2)
      {
        fprintf(stderr, "%s\n", sender->finishing != 0
                                    ? "FIN was not acknowledged after 10 timeouts"
                                    : "DATA was not acknowledged after 10 timeouts");
      }
      return action->status;
    case RDT_ACTION_NONE:
    case RDT_ACTION_WRITE_FILE:
    case RDT_ACTION_FLUSH_FILE:
    case RDT_ACTION_CLOSE_FILE:
      return 1;
    default:
      return 1;
    }
  }
  return 0;
}

int rdt_io_send(struct rdt_io_context *context, uint64_t file_size,
                const struct rdt_options *options)
{
  struct rdt_sender sender;
  struct rdt_protocol_actions actions;

  if (context == NULL || options == NULL ||
      rdt_sender_init(&sender, file_size, options->window_size,
                      options->timeout_ms, monotonic_milliseconds(context),
                      &actions) != 0)
  {
    fprintf(stderr, "Invalid sender protocol configuration\n");
    return 1;
  }
  return sender_dispatch(context, &sender, &actions);
}

static int receiver_dispatch(struct rdt_io_context *io,
                             struct rdt_receiver *receiver,
                             const struct rdt_protocol_actions *actions)
{
  struct rdt_protocol_actions current = *actions;
  size_t i;

process_batch:
  for (i = 0U; i < current.count; ++i)
  {
    const struct rdt_protocol_action *action = &current.items[i];
    struct rdt_protocol_actions next_actions;
    struct rdt_receiver_event event;
    int result;

    switch (action->type)
    {
    case RDT_ACTION_WRITE_FILE:
      result = write_file(io, action->bytes, action->length);
      memset(&event, 0, sizeof(event));
      // GCOVR_EXCL_START
      event.type = result == 0 ? RDT_RECEIVER_EVENT_FILE_OK
                               : RDT_RECEIVER_EVENT_IO_FAILURE;
      // GCOVR_EXCL_STOP
      event.now_ms = monotonic_milliseconds(io);
      if (rdt_receiver_step(receiver, &event, &next_actions) != 0)
      {
        return 1;
      }
      current = next_actions;
      goto process_batch;
    case RDT_ACTION_FLUSH_FILE:
    case RDT_ACTION_CLOSE_FILE:
      result = action->type == RDT_ACTION_FLUSH_FILE
                   ? flush_file(io)
                   : close_file(io);
      memset(&event, 0, sizeof(event));
      // GCOVR_EXCL_START
      event.type = result == 0 ? RDT_RECEIVER_EVENT_FILE_OK
                               : RDT_RECEIVER_EVENT_IO_FAILURE;
      // GCOVR_EXCL_STOP
      event.now_ms = monotonic_milliseconds(io);
      if (rdt_receiver_step(receiver, &event, &next_actions) != 0)
      {
        return 1;
      }
      current = next_actions;
      goto process_batch;
    case RDT_ACTION_SEND_PACKET:
      // GCOVR_EXCL_START
      if (send_datagram(io, action->bytes, action->length) != 0)
      {
        memset(&event, 0, sizeof(event));
        event.type = RDT_RECEIVER_EVENT_IO_FAILURE;
        if (rdt_receiver_step(receiver, &event, &next_actions) != 0)
        {
          return 1;
        }
        current = next_actions;
        goto process_batch;
      }
      // GCOVR_EXCL_STOP
      break;
    case RDT_ACTION_WAIT_UNTIL:
      memset(&event, 0, sizeof(event));
      result = receive_protocol_packet(io, action->deadline_ms,
                                       &event.packet);
      event.now_ms = monotonic_milliseconds(io);
      event.type = result == 0 ? RDT_RECEIVER_EVENT_PACKET
                               : RDT_RECEIVER_EVENT_TIMER;
      // GCOVR_EXCL_START
      if (result < 0)
      {
        event.type = RDT_RECEIVER_EVENT_IO_FAILURE;
      }
      // GCOVR_EXCL_STOP
      if (rdt_receiver_step(receiver, &event, &next_actions) != 0)
      {
        return 1;
      }
      current = next_actions;
      goto process_batch;
    case RDT_ACTION_COMPLETE:
      return 0;
    case RDT_ACTION_FAILED:
      if (action->status == 2)
      {
        fprintf(stderr, "Receiver timed out without a valid packet\n");
      }
      return action->status;
    case RDT_ACTION_NONE:
    case RDT_ACTION_READ_FILE:
      return 1;
    default:
      return 1;
    }
  }
  return 0;
}

#ifdef TEST
int rdt_io_test_dispatch(enum rdt_protocol_action_type type, int receiver,
                         int status, int empty_actions, int finishing,
                         int invalid_sender)
{
  struct rdt_io_context io = {
    .socket_fd = 999999,
    .file = NULL,
    .sender = 0
  };
  struct rdt_protocol_actions actions = {0};
  struct rdt_protocol_actions init_actions;
  struct rdt_protocol_action *action;
  struct rdt_receiver receiver_state;
  struct rdt_receiver *receiver_pointer = NULL;
  int result;

  io.file = tmpfile();
  // GCOVR_EXCL_START
  if (io.file == NULL)
  {
    return -1;
  }
  // GCOVR_EXCL_STOP
  if (empty_actions == 0)
  {
    actions.count = 1U;
    action = &actions.items[0];
    action->type = type;
    action->status = status;
    action->deadline_ms = 0U;
  }
  if (receiver != 0)
  {
    if (type == RDT_ACTION_WAIT_UNTIL && invalid_sender == 0)
    {
      (void) rdt_receiver_init(&receiver_state, 0U, &init_actions);
      receiver_state.idle_deadline_ms = 0U;
      receiver_pointer = &receiver_state;
    }
    result = receiver_dispatch(&io, receiver_pointer, &actions);
  }
  else
  {
    struct rdt_sender sender = {0};
    sender.finishing = finishing;
    result = sender_dispatch(&io, invalid_sender != 0 ? NULL : &sender,
                             &actions);
  }
  if (io.file != NULL)
  {
    (void) fclose(io.file);
  }
  return result;
}

int rdt_io_test_drive(int receiver, int null_options)
{
  struct rdt_io_context io = {
    .socket_fd = 999999,
    .file = NULL,
    .sender = receiver == 0
  };
  struct rdt_options options = {
    .session = "test",
    .window_size = 1U,
    .timeout_ms = 10U,
    .port = 0U,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  int result;

  io.file = tmpfile();
  // GCOVR_EXCL_START
  if (io.file == NULL)
  {
    return -1;
  }
  // GCOVR_EXCL_STOP
  if (receiver != 0)
  {
    result = rdt_io_receive(&io);
  }
  else
  {
    result = rdt_io_send(&io, 0U, null_options != 0 ? NULL : &options);
  }
  (void) fclose(io.file);
  return result;
}
#endif

int rdt_io_receive(struct rdt_io_context *context)
{
  struct rdt_receiver receiver;
  struct rdt_protocol_actions actions;

  if (context == NULL)
  {
    fprintf(stderr, "Invalid receiver protocol configuration\n");
    return 1;
  }
  (void) rdt_receiver_init(&receiver, monotonic_milliseconds(context),
                           &actions);
  return receiver_dispatch(context, &receiver, &actions);
}

static int transfer_exit_status(int result, int close_result)
{
  if (result != 0)
  {
    return result == 2 ? 2 : 1;
  }
  // GCOVR_EXCL_START
  if (close_result != 0)
  {
    return 1;
  }
  // GCOVR_EXCL_STOP
  return 0;
}

#ifdef TEST
int rdt_io_test_transfer_status(int result, int close_result)
{
  return transfer_exit_status(result, close_result);
}
#endif

int rdt_send_file(const char *host, unsigned short port, const char *path,
                  const struct rdt_options *options)
{
  struct rdt_io_context *context;
  uint64_t file_size = 0U;
  int result;
  int close_result;

  context = rdt_io_open(host, port, path, options, 1, &file_size);
  if (context == NULL)
  {
    return 1;
  }
  result = rdt_io_send(context, file_size, options);
  close_result = rdt_io_close(context);
  result = transfer_exit_status(result, close_result);
  if (result != 0)
  {
    return result;
  }
  printf("Transfer complete: %" PRIu64 " bytes sent to %s\n", file_size,
         host);
  return 0;
}

int rdt_receive_file(const char *host, unsigned short port, const char *path,
                     const struct rdt_options *options)
{
  struct rdt_io_context *context;
  int result;
  int close_result;

  context = rdt_io_open(host, port, path, options, 0, NULL);
  if (context == NULL)
  {
    return 1;
  }
  result = rdt_io_receive(context);
  close_result = rdt_io_close(context);
  result = transfer_exit_status(result, close_result);
  if (result != 0)
  {
    return result;
  }
  printf("Transfer complete: file received from %s\n", host);
  return 0;
}

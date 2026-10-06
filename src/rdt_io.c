#define _POSIX_C_SOURCE 200809L

#include "rdt_io.h"

#include "rdt.h"

#include <errno.h>
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
  if (status != 0) // GCOVR_EXCL_START
  {
    fprintf(stderr, "Cannot resolve relay '%s': %s\n", host,
            gai_strerror(status));
    return -1;
  } // GCOVR_EXCL_STOP

  for (address = addresses; address != NULL; address = address->ai_next)
  {
    socket_fd = socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol);
    if (socket_fd < 0) // GCOVR_EXCL_START
    {
      continue;
    } // GCOVR_EXCL_STOP
    if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0)
    {
      break;
    }
    // GCOVR_EXCL_START
    (void) close(socket_fd);
    socket_fd = -1;
    // GCOVR_EXCL_STOP
  }
  freeaddrinfo(addresses);

  if (socket_fd < 0)
  {// GCOVR_EXCL_START
    fprintf(stderr, "Cannot connect UDP socket to relay '%s:%u': %s\n",
            host, (unsigned int) port, strerror(errno));
  }// GCOVR_EXCL_STOP
  return socket_fd;
}

static int send_datagram(void *context, const uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;
  ssize_t sent;

  do
  {
    sent = send(io->socket_fd, bytes, length, 0);
  } while (sent < 0 && errno == EINTR);

  if (sent < 0 || (size_t) sent != length)
  { // GCOVR_EXCL_START
    if (sent >= 0)
    {
      fprintf(stderr, "Short UDP datagram send\n");
    }
    else
    {
      fprintf(stderr, "UDP send failed: %s\n", strerror(errno));
    }
    return -1;
  } // GCOVR_EXCL_STOP
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

  do
  {
    result = poll(&descriptor, 1U, timeout_ms);
  } while (result < 0 && errno == EINTR);

  if (result < 0)
  { // GCOVR_EXCL_START
    fprintf(stderr, "UDP wait failed: %s\n", strerror(errno));
    return -1;
  } // GCOVR_EXCL_STOP
  if (result == 0)
  {
    return 0;
  }
  if ((descriptor.revents & POLLIN) != 0)
  {
    return 1;
  }
  // GCOVR_EXCL_START
  fprintf(stderr, "UDP socket reported an error while waiting\n");
  return -1;
  // GCOVR_EXCL_STOP
}

static int receive_datagram(void *context, uint8_t *packet, size_t capacity,
                            size_t *length, int timeout_ms)
{
  struct rdt_io_context *io = context;
  int ready = wait_readable(io->socket_fd, timeout_ms);
  ssize_t received;

  if (ready <= 0)
  {
    return ready == 0 ? 1 : -1;
  }
  do
  {
    received = recv(io->socket_fd, packet, capacity, 0);
  } while (received < 0 && errno == EINTR);

  if (received < 0)
  { // GCOVR_EXCL_START
    fprintf(stderr, "UDP receive failed: %s\n", strerror(errno));
    return -1;
  } // GCOVR_EXCL_STOP
  *length = (size_t) received;
  return 0;
}

static uint64_t monotonic_milliseconds(void *context)
{
  struct timespec now;
  (void) context;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not read monotonic clock: %s\n", strerror(errno));
    return 0U;
  } // GCOVR_EXCL_STOP
  return (uint64_t) now.tv_sec * UINT64_C(1000) +
         (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static int read_file(void *context, uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;

  if (fread(bytes, 1U, length, io->file) != length)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not read input file: %s\n",
            ferror(io->file) != 0 ? strerror(errno) : "unexpected end of file");
    return -1;
  } // GCOVR_EXCL_STOP
  return 0;
}

static int write_file(void *context, const uint8_t *bytes, size_t length)
{
  struct rdt_io_context *io = context;

  if (fwrite(bytes, 1U, length, io->file) != length)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not write output file: %s\n", strerror(errno));
    return -1;
  } // GCOVR_EXCL_STOP
  return 0;
}

static int flush_file(void *context)
{
  struct rdt_io_context *io = context;

  if (fflush(io->file) != 0)
  {// GCOVR_EXCL_START
    fprintf(stderr, "Could not flush output file: %s\n", strerror(errno));
    return -1;
  }// GCOVR_EXCL_STOP
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
  if (hello_length < 0 || (size_t) hello_length >= sizeof(hello))
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not construct relay HELLO message\n");
    return -1;
  } // GCOVR_EXCL_STOP

  /* HELLO and its reply are not impaired; retries cover UDP loss. */
  for (attempt = 0; attempt < HELLO_ATTEMPTS; ++attempt)
  {
    uint8_t response[256];
    size_t response_length = 0U;
    int ready;
    ssize_t received;

    if (send_datagram(io, (const uint8_t *) hello,
                      (size_t) hello_length) != 0)
    {
      return -1;
    }
    ready = wait_readable(io->socket_fd, HELLO_TIMEOUT_MS);
    if (ready < 0)
    {// GCOVR_EXCL_START
      return -1;
    }// GCOVR_EXCL_STOP
    if (ready == 0)
    {// GCOVR_EXCL_START
      continue;
    }// GCOVR_EXCL_STOP
    do
    {
      received = recv(io->socket_fd, response, sizeof(response) - 1U, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
    {// GCOVR_EXCL_START
      fprintf(stderr, "UDP receive failed: %s\n", strerror(errno));
      return -1;
    }// GCOVR_EXCL_STOP
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
    {// GCOVR_EXCL_START
      fprintf(stderr, "Unexpected relay HELLO response: %s\n", response);
    }// GCOVR_EXCL_STOP
    return -1;
  }
  // GCOVR_EXCL_START
  fprintf(stderr, "Relay did not answer HELLO after %d attempts\n",
          HELLO_ATTEMPTS);
  return -1;
  // GCOVR_EXCL_STOP
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
  {// GCOVR_EXCL_START
    fprintf(stderr, "Invalid endpoint or relay options\n");
    return NULL;
  }// GCOVR_EXCL_STOP

  io = calloc(1U, sizeof(*io));
  if (io == NULL)
  {// GCOVR_EXCL_START
    fprintf(stderr, "Could not allocate I/O context: %s\n", strerror(errno));
    return NULL;
  }// GCOVR_EXCL_STOP
  io->socket_fd = -1;
  io->sender = sender;

  if (sender != 0)
  {
    io->file = fopen(path, "rb");
    if (io->file == NULL)
    {
      fprintf(stderr, "Cannot open input file '%s': %s\n", path,
              strerror(errno));
      rdt_io_close(io);
      return NULL;
    }
    if (fstat(fileno(io->file), &file_status) != 0 ||
        !S_ISREG(file_status.st_mode) || file_status.st_size < 0)
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
  if (io->socket_fd < 0)
  {// GCOVR_EXCL_START
    rdt_io_close(io);
    return NULL;
  }// GCOVR_EXCL_STOP
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

void rdt_io_get_protocol_io(struct rdt_io_context *context,
                            struct rdt_protocol_io *protocol_io)
{
  if (protocol_io == NULL)
  {
    return;
  }
  memset(protocol_io, 0, sizeof(*protocol_io));
  if (context == NULL)
  {
    return;
  }
  protocol_io->context = context;
  protocol_io->send_datagram = send_datagram;
  protocol_io->receive_datagram = receive_datagram;
  protocol_io->now_ms = monotonic_milliseconds;
  protocol_io->read_file = read_file;
  protocol_io->write_file = write_file;
  protocol_io->flush_file = flush_file;
}

int rdt_io_close(struct rdt_io_context *context)
{
  int result = 0;

  if (context == NULL)
  {
    return 0;
  }
  if (context->file != NULL && fclose(context->file) != 0)
  { // GCOVR_EXCL_START
    fprintf(stderr, "Could not close transfer file: %s\n", strerror(errno));
    result = -1;
  } // GCOVR_EXCL_STOP
  if (context->socket_fd >= 0 && close(context->socket_fd) != 0)
  {// GCOVR_EXCL_START
    fprintf(stderr, "Could not close relay socket: %s\n", strerror(errno));
    result = -1;
  }// GCOVR_EXCL_STOP
  free(context);
  return result;
}

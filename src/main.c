#define _POSIX_C_SOURCE 200809L

#include "rdt.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

#define DEFAULT_PORT 4250
#define DEFAULT_WINDOW 8U
#define DEFAULT_TIMEOUT_MS 250U
#define MAX_TIMEOUT_MS 60000U

static void print_usage(FILE *stream)
{
  fprintf(stream,
          "Usage: myapp send -s <session> [-w window] [-T timeout-ms] "
          "[-l loss]\n"
          "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
          "       myapp recv -s <session> [-p port] <relay> <file>\n"
          "\n"
          "  -s <session>     session name shared by the sender and the receiver\n"
          "  -w <window>      Go-Back-N window size in packets, 1 to 64 "
          "(default: 8)\n"
          "  -T <timeout-ms>  retransmission timeout in milliseconds "
          "(default: 250)\n"
          "  -l <loss>        probability the relay drops a packet "
          "(default: 0)\n"
          "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
          "  -d <dup>         probability the relay duplicates a packet "
          "(default: 0)\n"
          "  -p <port>        relay port (default: 4250)\n"
          "  <relay>          host name or address of the relay\n"
          "  <file>           file to send, or file to write what is received\n");
}

static int parse_unsigned(const char *text, unsigned long maximum,
                          unsigned long *value)
{
  char *end = NULL;
  unsigned long parsed;

  if (text == NULL || text[0] == '\0' || text[0] == '-')
  {
    return -1;
  }

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
  {
    return -1;
  }

  *value = parsed;
  return 0;
}

static int parse_probability(const char *text, double *value)
{
  char *end = NULL;
  double parsed;

  if (text == NULL || text[0] == '\0')
  {
    return -1;
  }

  errno = 0;
  parsed = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) ||
      parsed < 0.0 || parsed > 0.5)
  {
    return -1;
  }

  *value = parsed;
  return 0;
}

static int parse_send(int argc, char **argv)
{
  struct rdt_options options = {
    .window_size = DEFAULT_WINDOW,
    .timeout_ms = DEFAULT_TIMEOUT_MS,
    .port = DEFAULT_PORT,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  int option;
  int result;

  optind = 2;
  while ((option = getopt(argc, argv, "s:w:T:l:c:d:p:")) != -1)
  {
    unsigned long parsed;
    switch (option)
    {
      case 's':
        options.session = optarg;
        break;
      case 'w':
        if (parse_unsigned(optarg, 64UL, &parsed) != 0 || parsed == 0UL)
        {
          fprintf(stderr, "Invalid window size: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        options.window_size = (unsigned int) parsed;
        break;
      case 'T':
        if (parse_unsigned(optarg, MAX_TIMEOUT_MS, &parsed) != 0 ||
            parsed == 0UL)
        {
          fprintf(stderr, "Invalid timeout: %s (expected 1-%u ms)\n",
                  optarg, MAX_TIMEOUT_MS);
          print_usage(stderr);
          return 2;
        }
        options.timeout_ms = (unsigned int) parsed;
        break;
      case 'l':
        if (parse_probability(optarg, &options.loss) != 0)
        {
          fprintf(stderr, "Invalid loss probability: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        break;
      case 'c':
        if (parse_probability(optarg, &options.corrupt) != 0)
        {
          fprintf(stderr, "Invalid corruption probability: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        break;
      case 'd':
        if (parse_probability(optarg, &options.duplicate) != 0)
        {
          fprintf(stderr, "Invalid duplication probability: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        break;
      case 'p':
        if (parse_unsigned(optarg, 65535UL, &parsed) != 0 || parsed == 0UL)
        {
          fprintf(stderr, "Invalid relay port: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        options.port = (unsigned short) parsed;
        break;
      default:
        print_usage(stderr);
        return 2;
    }
  }

  if (options.session == NULL || optind + 2 != argc)
  {
    print_usage(stderr);
    return 2;
  }
  result = rdt_validate_session(options.session);
  if (result != 0)
  {
    fprintf(stderr,
            "Invalid session: use 1-32 characters from a-z, 0-9, and '-'\n");
    print_usage(stderr);
    return 2;
  }

  return rdt_send_file(argv[optind], options.port, argv[optind + 1], &options);
}

static int parse_receive(int argc, char **argv)
{
  struct rdt_options options = {
    .window_size = DEFAULT_WINDOW,
    .timeout_ms = DEFAULT_TIMEOUT_MS,
    .port = DEFAULT_PORT,
    .loss = 0.0,
    .corrupt = 0.0,
    .duplicate = 0.0
  };
  int option;

  optind = 2;
  while ((option = getopt(argc, argv, "s:p:")) != -1)
  {
    unsigned long parsed;
    switch (option)
    {
      case 's':
        options.session = optarg;
        break;
      case 'p':
        if (parse_unsigned(optarg, 65535UL, &parsed) != 0 || parsed == 0UL)
        {
          fprintf(stderr, "Invalid relay port: %s\n", optarg);
          print_usage(stderr);
          return 2;
        }
        options.port = (unsigned short) parsed;
        break;
      default:
        print_usage(stderr);
        return 2;
    }
  }

  if (options.session == NULL || optind + 2 != argc)
  {
    print_usage(stderr);
    return 2;
  }
  if (rdt_validate_session(options.session) != 0)
  {
    fprintf(stderr,
            "Invalid session: use 1-32 characters from a-z, 0-9, and '-'\n");
    print_usage(stderr);
    return 2;
  }

  return rdt_receive_file(argv[optind], options.port, argv[optind + 1],
                          &options);
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    print_usage(stderr);
    return 0;
  }
  if (strcmp(argv[1], "send") == 0)
  {
    return parse_send(argc, argv);
  }
  if (strcmp(argv[1], "recv") == 0)
  {
    return parse_receive(argc, argv);
  }

  print_usage(stderr);
  return 2;
}

# RDT client and packet protocol

The client communicates with an external UDP relay. Both peers bind an
ephemeral local port, send the relay a plain-text registration from that same
socket, and use that socket for all transfer packets. The receiver must register
first. The relay forwards registered peers' datagrams without interpreting their
binary contents.

## Commands

```text
myapp send -s <session> [-w window] [-T timeout-ms]
           [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
myapp recv -s <session> [-p port] <relay> <file>
```

The default relay port is 4250, sender window is 8, and retransmission timeout
is 250 ms. Window size is 1-64, timeout is 1-60000 ms, and impairment
probabilities are 0-0.5. The session name must be 1-32 lowercase letters,
digits, or hyphens. Sender impairment values configure the external relay and
are not locally simulated by the client.

Before file data moves, the receiver sends `HELLO <session> recv`; the sender
sends `HELLO <session> send <loss> <corrupt> <dup>`. The HELLO contains no
newline. The client waits up to one second for a reply and retries at most five
times. `ERR <reason>` is reported to the user.

## Implementation layers

The C implementation has three layers:

- `rdt.c` validates session names and encodes/decodes the packet header and
  checksum using pure byte-buffer functions. `rdt_checksum` is also tested
  against RFC 1071's published vector and an odd-length input.
- `rdt_protocol.c` contains pure sender and receiver state machines. The
  `rdt_sender_step` and `rdt_receiver_step` functions consume explicit packet,
  timer, and file-completion events and return actions such as sending a packet,
  reading or writing file bytes, or waiting until a monotonic deadline. They do
  not access sockets, clocks, or files.
- `rdt_io.c` is the I/O driver: it performs relay HELLO, uses UDP and `poll`,
  reads the monotonic clock, carries out returned file/network actions, and
  feeds the resulting events back into the state machines.

Protocol tests drive the event functions directly with explicit timestamps and
in-memory packets, so loss and timeout behavior can be controlled without a
relay or real clock. Five fixed-seed in-memory transfer runs independently
drop, corrupt, and duplicate packets in both directions and check the
delivered file bytes against the source.

## Binary datagrams

Every packet has the following header followed immediately by its payload.
All 16- and 32-bit integers use network byte order.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 1 | Type: `0` DATA, `1` ACK, or `2` FIN |
| 1 | 1 | Reserved; must be zero |
| 2 | 2 | Internet checksum (16-bit one's-complement checksum) |
| 4 | 4 | Sequence number |
| 8 | 2 | Payload length in bytes |
| 10 | 0-1024 | Payload |

The checksum covers the complete datagram with the checksum field zeroed.
Invalid types, reserved values, lengths, or checksums are discarded.

DATA sequence numbers start at zero. The sender maintains at most the
configured number of unacknowledged chunks. ACK sequence numbers are cumulative:
they identify the next DATA sequence expected by the receiver. Only in-order
DATA is written; duplicate and out-of-order datagrams are discarded and elicit
the current cumulative ACK. When the oldest outstanding DATA times out, the
sender retransmits its entire outstanding window. The sender gives up with
status 2 after ten consecutive timeouts without an advancing ACK; an ACK that
moves the base resets this timeout count.

While awaiting transfer packets, the receiver exits with status 2 after 30
seconds without receiving a valid RDT datagram. Valid packets restart this idle
timer; malformed or checksum-invalid datagrams do not.

The final chunk may contain fewer than 1024 bytes. FIN uses the next DATA
sequence number. The receiver accepts FIN only when `FIN sequence == expected`,
flushes and closes the output file, increments `expected`, and sends ACK
`expected`. It then lingers for two seconds, responding to repeated FIN packets
with the same ACK before exiting successfully. This gives the sender time to
recover if the final ACK was lost. An empty file uses FIN sequence zero and
final ACK sequence one.

The sender retransmits FIN after each timeout and returns exit status 2 after
ten timeouts without the final ACK.

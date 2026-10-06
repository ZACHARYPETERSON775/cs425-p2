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

The C implementation separates the wire/protocol concerns from operating
system I/O:

- `rdt.c` validates session names and encodes/decodes the packet header and
  checksum. Its public send/receive functions compose the other layers.
- `rdt_protocol.c` runs the Go-Back-N sender and receiver state machines using
  injected datagram, clock, and file callbacks declared in `rdt_protocol.h`.
- `rdt_io.c` implements those callbacks with UDP sockets, `poll`, the monotonic
  clock, the relay HELLO exchange, and `FILE` operations.

This lets protocol behavior be exercised through alternate callback
implementations without binding it to the socket or filesystem implementation.

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
sender retransmits its entire outstanding window.

The final chunk may contain fewer than 1024 bytes. FIN uses the next DATA
sequence number and carries the sender's timeout as a two-byte payload, allowing
the receiver to linger long enough to answer a retransmitted FIN if its ACK was
lost. The sender retransmits FIN after each timeout and returns exit status 2
after ten timeouts without the final ACK. The receiver accepts FIN only after
receiving all preceding DATA, responds with ACK sequence `FIN sequence + 1`, and
lingers for ten sender timeout intervals plus one second to cover FIN retries.
This also supports an empty file: its FIN sequence is zero.

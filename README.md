# Project 3 - Reliable Data Transfer

- Name: Zachary Peterson
- Email: zacharypeterson775@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

There are no known bugs at this time. We have 100% code coverage across all
functions and all functions have unit tests. Only branchs that result in
library or system calls are excluded in the code coverage report.

## Experience

AI make this project significatly easier than I originaly though it was going to be.
I had fun but have run out of time to write a proper reflection.

## Analysis

| Window | Loss | Corrupt | Dup | Mean Time | KiB/s   |
|--------|------|---------|-----|-----------|---------|
| 1      | 0    | 0       | 0   | 103.305s  | 9.912   |
| 16     | 0    | 0       | 0   | 6.644s    | 154.124 |
| 1      | 0.05 | 0       | 0   | 130.547s  | 7.844   |
| 16     | 0.05 | 0       | 0   | 23.955s   | 42.747  |

1. From the window 1, no loss run, compute the round trip time your sender actually saw. (It sent 1025 packets and waited one round trip for each.) The relay adds 100 ms. Where does the rest come from?

For a windows size of 1 with no loss, there was an observed mean RTT of 100.785ms. 
100ms of the RTT came from the relay, while additional delay was created while the
sender and reciver were reading, validating, and generating responses to incoming packets. 

2. Use that round trip time to explain the speedup at window 16. Is it close to 16 times? Why or why not?

For a windows size of 16 with no loss, there was an observed mean RTT of 6.482ms.
With the reduction of RTT to almost 1/16 when compared to the windows size 1, no loss run,
the transfer time was also reduced because the average time taken for any given packet to 
be send and acked was redueced.

3. Compared with its no loss time, why does 5% loss slow the window 16 run down so much more than the window 1 run? Think about what a timeout makes the sender resend in each case.

The 5% loss slowed the window 16 run significatly more than the window 1 run because when a packet was lost, the windows 16 run would have to retrasfer upto 16 packets while the windows 1 run only needed to retransmit 1 packet.

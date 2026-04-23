# Development

Level-IP is at a very alpha stage, has many hardcoded values and is not really intuitive to develop on. 

This document aims to provide information on the current features, roadmap and overall development routine. 

# Debugging

Build Level-IP with `make debug`. It adds debug symbols and by default, enables Google's Address Sanitizer.

## Debug Output

When built with `make debug`, `lvl-ip` becomes chatty and outputs debug statements. You can enable/disable different component debug output with macros defined in headers.

For example, enabling socket-specific output:

    make clean
    CFLAGS+=-DDEBUG_SOCKET make debug 

## Debugging Networking

Use `tcpdump` with the IP address you're using, e.g.:

    $ tcpdump -i any host 10.0.0.5 -n
    IP 10.0.0.5.12000 > 10.0.0.5.8000: Flags [S], seq 1525252, win 512, length 0
    IP 10.0.0.5.8000 > 10.0.0.5.12000: Flags [S.], seq 1332068674, ack 1525253, win 29200, options [mss 1460], length 0
    IP 10.0.0.5.12000 > 10.0.0.5.8000: Flags [.], ack 1, win 512, length 0

Together with the verbose `lvl-ip` output, you can troubleshoot behaviour and spot patterns.

## Tracing Program Code

Simply run `gdb` with your favorite interface.

Refer to https://sourceware.org/gdb/current/onlinedocs/gdb/Threads.html for debugging with threads.

## Debugging Memory Allocation and Use

One of the useful debugging aids so far has been Address Sanitizer by Google. It is built in to newer GCC versions, and it is activated with `-fsanitize=address`. Sure enough, `make debug` enables this by default.

https://github.com/google/sanitizers/wiki/AddressSanitizer

## Debugging Concurrency

Level-IP uses multiple threads with shared data structures, therefore programming errors like race conditions are easy to introduce.

Thread Sanitizer by Google is also built in to newer GCCs, which helps pinpointing concurrent access to variables without proper guards.

https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual

# Coding Style

The foremost aim of Level-IP is to be an educational project on networking. Hence, source code readability should be focused on when developing Level-IP.

TODO: Actual style guidelines, so far I have been just winging it.

# Features

First and foremost, Level-IP aims to be just an introduction to TCP/IP stacks. Hence, convenient features are prioritized over e.g. raw performance improvements. 

## Current Features

* One hardcoded interface/netdev (IP 10.0.0.5)
* One hardcoded socket
* Ethernet II frame handling
* ARP request/reply, simple caching
* ICMP pings and replies 
* IPv4 packet handling, checksum
* One hardcoded route table with default netdevice
* TCPv4 Handshake
* TCP data transmission
* TCP RFC793 "Segment Arrives"
* TCP RFC6298 Retransmission calculation
* TCP RFC793 User Timeout

## Upcoming features

* IP Fragmentation
* IP/ICMP Diagnostics
* TCP Window Management
* TCP Silly Window Syndrome Avoidance
* TCP Zero-Window Probes
* TCP Congestion Control
* TCP Selective Acknowledgments (SACK)
* Server socket API calls (bind, accept...)
* Raw Socket (for arping, ping..)
* 'select' socket API call
* ...

## FEC Module (GF(256) Polynomial)

An erasure-recovery FEC module is available via [include/fec.h](include/fec.h) and [src/fec.c](src/fec.c).

For a full algorithm walkthrough, see [Documentation/fec-algorithm.md](fec-algorithm.md).

The design follows the original polynomial-evaluation view:

* Source packets are treated as coefficients $a_0..a_{k-1}$ of $P(x)=\sum_{i=0}^{k-1} a_i x^i$.
* A redundant packet is generated as one extra evaluation point $P(x_r)$ over GF(256).
* If one source packet is lost, it is recovered from the surviving coefficients and $P(x_r)$.
* Full interpolation from arbitrary points is provided to reconstruct all coefficients.

All arithmetic is over GF(256) using log/exp lookup tables (primitive polynomial 0x11d).

### API

* `fec_rs_init()` initializes GF(256) tables.
* `fec_poly_encode_redundant()` generates one redundant packet at chosen `x_r`.
* `fec_poly_recover_missing_coefficient()` recovers one missing source coefficient packet.
* `fec_poly_interpolate_coefficients()` reconstructs coefficient packets from $k$ points.

### Quick Verification

Run:

    make fec-test

This builds and executes [tests/fec-selftest.c](tests/fec-selftest.c), which checks:

* Single missing-packet recovery from one redundant packet.
* Full coefficient reconstruction by interpolation from four GF(256) points.

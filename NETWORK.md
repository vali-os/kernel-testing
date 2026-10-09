# Socket regression tests in QEMU

The opt-in `nettest` service exercises the real libc/libos APIs, netd RPC,
notification queues, and shared-memory pipes. It is excluded by default.

From an already configured Vali checkout:

```sh
cmake -S . -B build -DVALI_NET_TESTS=ON
make -C build -j4
make -C build mkimage
tools/run-qemu.sh build/disk.img
```

Look in `qserial.txt` for `NETTEST ALL PASS`. A failed assertion prints
`NETTEST FAIL` and its source line. A missing completion marker is a failure
or hang, not a passing test. The test service does not shut down QEMU.

Coverage:

- Invalid socket types and unsupported Internet domains.
- Empty option/address error replies for valid and invalid handles.
- Truncated, oversized, unterminated, wrong-family, and inconsistent addresses.
- Bidirectional stream socket pairs and close.
- Directional shutdown rejection preserves both pipes; invalid close operations
  and READ_WRITE never destroy a socket.
- Finite 96 KiB bursts through stream, datagram, and sequenced-packet sockets:
  stop sending before draining the full receive pipe, verify every byte, and
  check that another socket remains responsive while the receiver is full.
- Datagram delivery, malformed destination rejection, maximum-length names,
  and expansion of short addresses without payload corruption.
- Bind/listen/connect/accept, including optional peer-address output, and traffic
  in both directions through accepted sockets.

To return to a normal image, configure with `-DVALI_NET_TESTS=OFF`, rebuild,
and run `make -C build mkimage` again. Disabling the option removes the test
service from the deployment directory.

These tests cover local sockets. IPv4/IPv6 and Ethernet integration remain
unfinished; rejecting those domains is intentional.

The separate [netadapter IPC suite](../docs/netadapter-ipc-tests.md) uses
`VALI_NET_ADAPTER_TESTS=ON` to run netd's v2 sessions against a software controller
in another process. It is independent of the local-socket tests above.

# Fixed-packet C2H receive FIFO

The driver can keep an AXI-Stream C2H engine running while an application
consumes completed packets through the existing `read()` interface. The
driver owns the coherent data ring and cyclic descriptors; applications do
not register buffers or report how many user buffers they allocate.

## Enable

Build with `POLLING=1`. With the device idle, load the module with:

```sh
packet_bytes=1024
fifo_slots=128
sudo insmod ./xdma-chr.ko c2h_fifo_frame_bytes="$packet_bytes" \
  c2h_fifo_slots="$fifo_slots"
```

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `c2h_fifo_slots` | 256 | Ring capacity in packets; 2..512, or 0 to disable FIFO |
| `c2h_fifo_frame_bytes` | 0 | Keeps legacy read at 0; explicit FIFO packet length is 64..65536 bytes, multiple of 64 |
| `c2h_fifo_credit_batch` | 0 | Auto selects min(32, slots); an explicit value must be 1..slots |

Choose packet_bytes to match the FPGA producer, not the application's read
count. The 1024-byte value above is an example. With packet_bytes unset or
zero, the driver retains the original per-read DMA path, whose transfer
request length comes from read's count. There was no 768-byte packet default
in that original path. A nonzero packet length opts into FIFO when slots is
also nonzero; invalid nonzero packet lengths fail FIFO reads with EINVAL.
Setting slots=0 explicitly keeps the legacy path regardless of packet length.

For the tested 768-byte FPGA protocol, the default ring has 256 slots:

```sh
sudo insmod ./xdma-chr.ko c2h_fifo_frame_bytes=768
# Override capacity at module load when needed:
sudo insmod ./xdma-chr.ko c2h_fifo_frame_bytes=768 c2h_fifo_slots=128
```

These are alternative module-load commands. The default of 256 is a slot
count, not a byte capacity; data memory is allocated only when FIFO starts.

Payload capacity is slots * packet_bytes. Descriptor storage is slots * 32
bytes. The example allocates 128 KiB of payload and 4 KiB of descriptors
per active streaming C2H engine; a 256-slot, 768-byte DiffTest configuration
allocates 192 KiB and 8 KiB respectively. Slots and packet length can be
selected independently within their supported ranges. With an 8-slot ring,
the default credit batch automatically becomes 8.

The parameters apply to all FIFO-enabled C2H streaming engines and are
read-only after module load. Stop readers and reload the module to resize;
there is no per-read reconfiguration. Allocation occurs on the first nonzero
read, rather than at module load. Configure larger application retention
pools separately; their memory is not part of the driver's coherent ring.

FIFO mode requires the hardware C2H descriptor-credit facility and packets
whose payload length is **exactly** `c2h_fifo_frame_bytes`, including the final
packet. This path does not inspect per-packet result records and cannot
validate variable packet lengths. Use the default, disabled mode for variable
length streaming traffic. FIFO applies to streaming C2H engines; H2C and
memory-mapped engines use their existing paths. An IRQ build rejects nonzero
FIFO reads with `EOPNOTSUPP`; it does not silently switch to another path.

## Application interface

```c
ssize_t received = read(fd, buffer, buffer_size);
```

Each read returns at most the remainder of one completed packet, even when
the requested count is larger. It can return as soon as that packet completes;
it does not wait for the entire ring to fill. A smaller count leaves the
remaining bytes of that packet for the next read. The caller must check the
returned byte count, as for other read interfaces.

- A zero-length read returns zero without starting DMA.
- An empty blocking read polls completion writeback and waits for a packet.
  Signals interrupt the wait. `c2h_timeout_ms` bounds each empty wait; zero
  means no timeout.
- An empty nonblocking read returns `EAGAIN`.
- A user-copy fault returns `EFAULT` without advancing the packet offset or
  releasing its credit. Some bytes may already have reached user memory.
- Invalid ring parameters return `EINVAL`; hardware credit support missing
  returns `EOPNOTSUPP`; engine/count errors return `EIO`.

The existing exclusive-open and concurrent-operation guards still apply.
Transfer submission, performance-test control and address-mode changes via
ioctl are rejected with `EBUSY` on FIFO-enabled engines. Read-only query
ioctls remain available. No new ioctl definitions are needed. For DiffTest,
leave the old experimental `DIFFTEST_XDMA_PROBE_FRAMES` setting unset.

## Ownership and completion

Descriptors form a cyclic chain without STOP. Their NEXT adjacency fields
are bounded by the next 64-descriptor block and ring end. Hardware credits
limit DMA reuse to available slots. Completion uses the existing 24-bit
descriptor-count writeback, with modular subtraction across counter rollover.

The CPU observes payload writes after completion, copies the completed
packet, and returns its credit only after all bytes have been consumed.
Credits accumulate up to the configured batch size; an empty ring flushes
any pending credits to avoid starving the producer. If the consumer stalls,
the hardware exhausts credits and applies AXI-Stream backpressure rather
than overwriting unread data.

Close clears RUN and waits for BUSY to clear before freeing DMA allocations.
If stop times out, the driver reports the error and retains DMA memory, since
the device may still access it. This safeguard does not guarantee recovery
from a wedged device. Normal close and reopen were validated; stop-timeout
fault injection and hot removal during an active FIFO are not covered.

Kernel start/close messages report the ring configuration, consumed bytes
and packets, credit updates, peak completed packets, and empty-wait time.
These per-engine counters are diagnostics, not a new userspace ABI.

## DiffTest configuration

The ordinary DiffTest path opens the C2H node and repeatedly requests
sizeof(FpgaPackgeHead) bytes. Its generated batch size and host AXIS width
determine that structure's length; it does not set driver ring parameters
between reads. Set c2h_fifo_frame_bytes to the corresponding complete FPGA
packet length when loading the module. For the tested configuration:

```sh
sudo insmod ./xdma-chr.ko c2h_fifo_frame_bytes=768 c2h_fifo_slots=256
```

The read count only bounds how many bytes are copied by that call. A partial
read neither resizes the ring nor changes descriptor packet lengths. The
number of application buffers and the eight DiffTest batch elements in
FpgaPackgeHead do not tell the driver how many DMA slots to allocate.

## Validation

The 768-byte FPGA packet protocol was tested with 8 and 256 slots, delayed
consumers, partial reads, nonblocking reads, interrupted waits and user-copy
faults. Complete CoreMark, Linux + CoreMark, and segmented fork checking
were exercised. The fixed-packet assumption is essential to these results.

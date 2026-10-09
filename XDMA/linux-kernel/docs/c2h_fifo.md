# Fixed-packet C2H receive FIFO

The driver keeps an AXI-Stream C2H engine running while applications consume
completed packets through the existing `read()` interface. The driver owns
the coherent data ring and cyclic descriptors. Applications do not register
buffers or report how many user buffers they allocate.

## Configuration

Build with `POLLING=1`. With the device idle, load the module with:

```sh
sudo insmod ./xdma-chr.ko
# Alternative: override the default capacity at module load.
sudo insmod ./xdma-chr.ko c2h_fifo_slots=128
```

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `c2h_fifo_slots` | 256 | Ring capacity in packets; 2..512, or 0 to disable FIFO |
| `c2h_fifo_credit_batch` | 0 | Auto selects min(32, slots); an explicit value must be 1..slots |

There is no packet-length module parameter. The original driver obtained
transfer length from read's count; the FIFO derives its per-engine slot
length from the **first nonzero read**. That read must request the complete,
fixed FPGA packet length: 64..65536 bytes, a multiple of 64. Later reads can
consume a packet in parts, but do not resize descriptors. Close and reopen
the device to select another packet length. A zero-length read does not
initialize the FIFO.

The default of 256 is a slot count, not a byte capacity. Payload capacity is
slots times the first read's count; descriptors occupy slots times 32 bytes.
A 256-slot, 768-byte DiffTest configuration allocates 192 KiB of payload and
8 KiB of descriptors per active streaming C2H engine. Allocation occurs on
the first nonzero read. With an 8-slot ring, the default credit batch is 8.

Slot count and credit batching apply to all FIFO-enabled engines and are
read-only after module load. Stop readers and reload the module to change
them. Each engine derives its own packet length from its first read. Larger
application retention pools are configured separately and are not part of
the driver's coherent ring.

FIFO requires hardware C2H descriptor credits and packets whose payload
length is **exactly** the first read's count, including the final packet.
This path uses descriptor-count writeback; it does not inspect per-packet
result records and cannot validate variable packet lengths. Load with
`c2h_fifo_slots=0` to retain the original per-read DMA path for such traffic.
FIFO applies to streaming C2H engines; H2C and memory-mapped engines use
their existing paths. IRQ builds reject enabled FIFO reads with
`EOPNOTSUPP`; load with `c2h_fifo_slots=0` to use legacy reads in an IRQ build.

## Application interface

```c
/* First nonzero read establishes the complete fixed packet length. */
ssize_t received = read(fd, buffer, packet_size);
```

Each read returns at most the remainder of one completed packet, even when
later read counts are larger. It returns as soon as that packet completes;
it does not wait for the entire ring to fill. A smaller count after
initialization leaves remaining packet bytes for the next read. The caller
must check the returned byte count, as for other read interfaces.

- A zero-length read returns zero without starting DMA.
- An empty blocking read polls completion writeback and waits for a packet.
  Signals interrupt the wait. `c2h_timeout_ms` bounds each empty wait; zero
  means no timeout.
- An empty nonblocking read returns `EAGAIN`.
- A user-copy fault returns `EFAULT` without advancing the packet offset or
  releasing its credit. Some bytes may already have reached user memory.
- Invalid ring parameters or initial read length return `EINVAL`; missing
  hardware credit support returns `EOPNOTSUPP`; engine/count errors return
  `EIO`.

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
from a wedged device. Normal close and reopen were validated on earlier
versions; stop-timeout fault injection and active hot removal are not covered.

Kernel start/close messages report ring configuration, consumed bytes and
packets, credit updates, peak completed packets, and empty-wait time. These
per-engine counters are diagnostics, not a new userspace ABI.

## DiffTest configuration

The ordinary DiffTest path opens the C2H node and repeatedly requests
`sizeof(FpgaPackgeHead)` bytes. Its generated batch size and host AXIS width
determine that structure's length. The first read therefore supplies the
FIFO packet length without any new DiffTest code or configuration between
reads. Load the driver normally for 256 slots, or override only slot count.

Subsequent read counts bound the bytes copied by that call. A partial read
neither resizes the ring nor changes descriptor packet lengths. The number
of application buffers and the eight DiffTest batch elements in
FpgaPackgeHead do not tell the driver how many DMA slots to allocate.

## Validation

Earlier FIFO versions tested the 768-byte FPGA packet protocol with 8 and
256 slots, delayed consumers, partial reads, nonblocking reads, interrupted
waits and user-copy faults. Complete CoreMark, Linux + CoreMark, and
segmented fork checking were exercised. Those versions used a hardcoded
or explicit packet length. They do not constitute hardware validation of
the latest first-read-derived sizing change. The fixed-packet assumption
is essential to all these results.

# Fixed-packet C2H receive FIFO

The driver can keep an AXI-Stream C2H engine running while an application
consumes completed packets through the existing `read()` interface. The
driver owns the coherent data ring and cyclic descriptors; applications do
not register buffers or report how many user buffers they allocate.

## Enable

Build with `POLLING=1`. With the device idle, load the module with:

```sh
sudo insmod ./xdma-chr.ko c2h_fifo_slots=256 c2h_fifo_credit_batch=32
```

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `c2h_fifo_slots` | 0 | Disabled at 0; enabled ring capacity is 2..512 packets |
| `c2h_fifo_frame_bytes` | 768 | Fixed packet length, 64..65536 bytes, multiple of 64 |
| `c2h_fifo_credit_batch` | 32 | Return credits after 1..slots packets are consumed |

The parameters are read-only after module load. For an 8-slot ring, also set
`c2h_fifo_credit_batch=8`. A 256-slot, 768-byte ring allocates 192 KiB of
payload storage and 8 KiB of descriptors per active streaming C2H engine.
Allocation occurs on the first nonzero read, rather than at module load.

FIFO mode requires the hardware C2H descriptor-credit facility and packets
whose payload length is **exactly** `c2h_fifo_frame_bytes`, including the final
packet. This path does not inspect per-packet result records and cannot
validate variable packet lengths. Use the default, disabled mode for variable
length streaming traffic. FIFO applies to streaming C2H engines; H2C and
memory-mapped engines use their existing paths. An IRQ build rejects nonzero
FIFO reads with `EOPNOTSUPP`; it does not silently switch to another path.

## Application interface

```c
ssize_t received = read(fd, buffer, 768);
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

## Validation

The 768-byte FPGA packet protocol was tested with 8 and 256 slots, delayed
consumers, partial reads, nonblocking reads, interrupted waits and user-copy
faults. Complete CoreMark, Linux + CoreMark, and segmented fork checking
were exercised. The fixed-packet assumption is essential to these results.

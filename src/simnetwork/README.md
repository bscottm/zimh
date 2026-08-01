# SIMNETWORK - Simulator Network Subsystem

## Overview

The `simnetwork` directory contains the network backend infrastructure for the ZIMH simulator. It provides a pluggable, multi-threaded ethernet device emulation layer that supports multiple network backends including PCAP, TAP/TUN, VDE, UDP, and NAT (via libslirp).

This subsystem abstracts the complexities of different network APIs into a unified interface, allowing simulated devices to send and receive ethernet frames regardless of the underlying transport mechanism.

## Architecture

### High-Level Design

```
+------------------+
|  Simulated NIC   |  (e.g., Ethernet controller in emulated system)
|  (sim_ether.h)   |
+--------+---------+
         |
         | eth_read() / eth_write()
         |
+--------v---------+
|   ETH_DEV        |  Device state, queues, backend pointer
+--------+---------+
         |
         | Backend dispatch via function pointers
         |
+--------v---------+
|  eth_backend_t   |  Discriminated union holding API-specific state
|  (eth_backends.h)|
+--------+---------+
         |
         +---> PCAP:   pcap_t handle
         +---> TAP:    socket fd
         +---> VDE:    VDECONN handle
         +---> UDP:    socket fd
         +---> NAT:    sim_slirp_network state
         +---> TEST:   ETH_TEST_BACKEND (for unit tests)
```

### Threading Model (when USE_READER_THREAD is defined)

The network subsystem uses a **producer-consumer model** with separate reader and writer threads:

```
                   +-------------------+
                   | Simulated Device  |
                   | (Main Thread)     |
                   +---------+---------+
                             |
              eth_read()     |     eth_write()
                   |         |         |
         +---------v-+     +-v---------v-+
         |  Reader   |     |   Writer    |
         |  Thread   |     |   Thread    |
         +-+---------+     +---------+---+
           |                         |
           | Enqueue                 | Dequeue
           | (lock-free)             | (lock-free)
           |                         |
    +------v------+           +------v------+
    | read_queue  |           |write_requests|
    | (sim_tailq) |           | (sim_tailq) |
    +-------------+           +-------------+
           |                         |
           |                         |
    +------v------+           +------v------+
    |   Backend   |           |   Backend   |
    | packet_read |           | write_packet|
    +-------------+           +-------------+
           |                         |
           v                         v
    +------------------------------------+
    |     Network (Wire/Virtual)         |
    +------------------------------------+
```

**Reader Thread:**
- Waits for incoming packets using poll()/select()/WaitForSingleObject()
- Reads packets from the backend
- Enqueues them to `read_queue` (lock-free SPSC queue)
- Optionally triggers async device poll via `sim_activate_abs()`

**Writer Thread:**
- Waits on condition variable for write requests
- Dequeues packets from `write_requests`
- Sends them via backend's `write_packet()` function
- Implements throttling if configured
- Returns buffers to freelist (batch optimized)

**State Machines:**
- Reader: INIT → RUNNING → SHUTDOWN/ERROR
- Writer: INIT → RUNNING → SHUTDOWN/ERROR

### Backend Dispatch System

Each backend implements a **function pointer interface** defined in `eth_backend_t`:

```
struct eth_backend_s {
    eth_api_t eth_api;              // Which backend is active
    
    // Function pointers (polymorphism in C):
    int  (*packet_wait)(backend, dev);       // Wait for packet arrival
    int  (*packet_read)(backend, dev);       // Read packet, queue for device
    bool (*before_packet_write)(backend, dev); // Pre-write setup (optional)
    int  (*write_packet)(dev, packet);       // Write one packet
    bool (*after_packet_write)(backend, dev);  // Post-write cleanup (optional)
    void (*reader_shutdown)(backend, dev);   // Reader cleanup (optional)
    void (*writer_shutdown)(backend, dev);   // Writer cleanup (optional)
    
    union {
        pcap_t *pcap;                   // PCAP handle
        VDECONN *vde;                   // VDE connection
        sim_slirp_network *slirp;       // SLiRP/NAT state
        SOCKET eth_socket;              // TAP/UDP socket
        ETH_TEST_BACKEND *test_backend; // Test harness
    } state;
};
```

**Dispatch Tables** (`eth_dispatch.c`):
- One reader/writer function per `eth_api_t` enum value
- Indexed dispatch: `eth_reader_dispatch_table[api_type]`
- No conditionals in hot path

### Packet Flow

**Receive Path:**
```
Network Wire
    |
    v
Backend packet_wait() blocks on poll/select
    |
    v
Backend packet_read() retrieves raw bytes
    |
    v
eth_process_received_packet() - filtering, validation
    |
    +---> Loopback check
    +---> Promiscuous/filter check
    +---> Runt padding (< 60 bytes)
    +---> IP checksum offload fixup
    +---> CRC generation (if needed)
    |
    v
eth_tailq_insert_data() - enqueue to read_queue
    |
    v
Simulated device calls eth_read() - dequeues packet
```

**Transmit Path:**
```
Simulated device calls eth_write()
    |
    v
Allocate ETH_WRITE_REQUEST from freelist
    |
    v
Copy packet data into request
    |
    v
Enqueue to write_requests queue
    |
    v
Signal writer_cond to wake writer thread
    |
    v
Writer thread dequeues request
    |
    +---> Throttling check (if enabled)
    +---> before_packet_write() hook
    +---> Backend write_packet()
    +---> after_packet_write() hook
    |
    v
Return request to freelist (batch optimization)
```

## Key Data Structures

### eth_packet (ETH_PACK)
Represents a single ethernet frame:
```c
struct eth_packet {
    uint8_t msg[ETH_FRAME_SIZE];  // Inline buffer for normal frames (1518 bytes)
    uint8_t *oversize;            // Heap allocation for jumbo frames
    uint32_t len;                 // Payload length (no CRC)
    uint32_t used;                // Bytes consumed (for chaining)
    int status;                   // TX/RX status code
    uint32_t crc_len;             // Length including CRC
};
```

### eth_item
Queue wrapper that adds metadata:
```c
struct eth_item {
    eth_item_type_t type;  // SETUP, LOOPBACK, or NORMAL
    struct eth_packet packet;
};
```

### eth_backend_t
See "Backend Dispatch System" above. Central abstraction for network backends.

### sim_slirp_network
NAT/SLiRP backend state (when HAVE_SLIRP_NETWORK is defined):
```c
struct sim_slirp {
    SlirpConfig slirp_config;      // libslirp configuration
    Slirp *slirp_cxn;              // libslirp context
    pthread_mutex_t libslirp_lock; // Mutex (libslirp is NOT thread-safe)
    
    // Socket tracking for poll/select:
    slirp_os_socket *lut;          // Lookup table of active sockets
    size_t lut_in_use;             // Number of active sockets
    
    #if SIM_USE_SELECT
        fd_set readfds, writefds, exceptfds;
        slirp_os_socket max_fd;
    #elif SIM_USE_POLL
        sim_pollfd_t *fds;         // Poll descriptor array
    #endif
    
    // Port forwarding:
    struct redir_tcp_udp *rtcp;    // TCP/UDP redirection list
    
    ETH_DEV *eth_dev;              // Associated ethernet device
};
```

**Critical: libslirp is NOT thread-safe**. The `libslirp_lock` mutex serializes all access:
- `before_slirp_send()` acquires the lock
- `after_slirp_send()` releases it
- Reader thread locks during `slirp_pollfds_poll()`

## File Descriptions

### Core Infrastructure

**eth_types.h**
- Defines all core types and constants
- `ETH_MAX_PACKET` (1514), `ETH_MAX_JUMBO_FRAME` (65536)
- `eth_api_t` enum: NONE, PCAP, TAP, VDE, UDP, NAT, TEST
- Forward declarations for opaque types

**eth_backends.h**
- `eth_backend_t` structure definition
- Backend-specific includes (pcap.h, libvdeplug.h, etc.)
- Function pointer typedefs for backend operations
- Platform-specific BPF header ordering (macOS 15.5+ compatibility)

**eth_dispatch.h / eth_dispatch.c**
- Reader/writer dispatch function types
- State machine enums: `eth_reader_status_t`, `eth_writer_state_t`
- Backend-specific reader/writer implementations:
  - `eth_reader_pcap()`, `eth_writer_pcap()`
  - `eth_reader_tap()`, `eth_writer_tap()`
  - `eth_reader_vde()`, `eth_writer_vde()`
  - `eth_reader_nat()`, `eth_writer_nat()`
  - `eth_reader_udp()`, `eth_writer_udp()`
  - `eth_reader_test()`, `eth_writer_test()`

### Threading Support

**eth_threads.h / eth_threads.c**
- Thread entry points: `_eth_reader()`, `_eth_writer()`
- Thread initialization and startup synchronization
- State machine implementations for reader/writer
- Wait/poll dispatch tables per backend type
- `eth_start_threads()` - creates and waits for both threads to be ready

**Key Functions:**
- `eth_reader_init()`: Sets thread name, CPU affinity, signals ready
- `eth_writer_init()`: Same for writer thread
- `eth_wait_pcap/tap/vde/udp/nat()`: Backend-specific poll/select wrappers
- `eth_reader_error_handler()`: Attempts recovery on read errors

### Packet Processing

**eth_pktreader.c**
- **Core packet ingress logic** (backend-agnostic)
- `eth_process_received_packet()`: Main entry point for all received packets
  - Loopback detection and handling
  - Address filtering (promiscuous, multicast, hash filter)
  - Runt packet padding to 60 bytes minimum
  - IP checksum offload fixup for locally-originated packets
  - CRC generation
  - Queue insertion

**Key Features:**
- **Checksum Offload Fixup**: Modern NICs offload IP/TCP/UDP checksum calculation. When packets loop back without traversing real hardware, we must compute checksums ourselves. See `eth_fix_ip_xsum_offload()`.
- **Jumbo Frame Segmentation**: Handles Large Send Offload (LSO) by fragmenting oversized frames. See `eth_fix_ip_jumbo_offload()`.
- **ECTP Loopback**: Processes Ethernet Configuration Test Protocol (0x9000) for diagnostics.

**eth_read.c**
- `eth_read()`: Simulator-side read function
- Dequeues packets from `read_queue` (threaded mode)
- Or dispatches directly to backend (polled mode)

**eth_queue.c**
- Adapter layer between `sim_tailq_t` (generic queue) and ethernet packets
- `eth_tailq_init/destroy/clear()`: Queue lifecycle
- `eth_tailq_insert/insert_data()`: Enqueue packets
- `eth_tailq_remove()`: Dequeue and free
- `ethq_item_free()`: Frees `eth_item` and any oversized buffer

### Device Enumeration

**eth_devices.c**
- **Native** network device enumeration (no PCAP dependency)
- `eth_devices_native()`: Platform-specific device discovery

**Windows Implementation:**
- Uses IP Helper API (`GetAdaptersAddresses`)
- Local heap allocation via `HeapCreate/HeapAlloc/HeapDestroy`
- Filters for Ethernet/802.11, operational interfaces only
- Returns PCAP-compatible device names: `\Device\NPF_{GUID}`

**Linux/Unix Implementation:**
- Uses `getifaddrs()` with `AF_PACKET` (Linux) or `AF_LINK` (BSD)
- Extracts MAC addresses to verify Ethernet interfaces
- Filters loopback and down interfaces

**Benefits:**
- No npcap.dll dependency for device listing
- Faster enumeration (no PCAP initialization overhead)
- Better OS integration

### Backend-Specific Code

**slirp/sim_slirp.h / slirp/sim_slirp.c**
- NAT/SLiRP backend implementation using libslirp
- Provides userspace TCP/IP stack with NAT
- Port forwarding: Guest TCP/UDP ports → Host ports
- IPv4: 10.0.2.0/24, IPv6: fd00:cafe:dead:beef::/64
- **Thread-Safety**: Mutex protects all libslirp calls
- Socket polling via `slirp_pollfds_poll()` callback mechanism

**slirp/slirp_poll.c**
- Socket registration/unregistration for libslirp
- Manages dynamic socket array for poll()/select()
- Callbacks: `register_poll_socket()`, `unregister_poll_socket()`

**unix_tap/eth_tap_open.c**
- TUN/TAP device initialization for Linux and BSD
- Linux: `/dev/net/tun` with `IFF_TAP | IFF_NO_PI`
- BSD: `/dev/tapN` direct device open
- macOS: Brings interface up via `SIOCGIFFLAGS/SIOCSIFFLAGS`

**vde/eth_vde_open.c**
- VDE (Virtual Distributed Ethernet) backend
- Connects to VDE switch via Unix socket
- `vde_open()` with optional port number specification

## Network Backend Comparison

| Backend | Description | Use Case | Thread-Safe |
|---------|-------------|----------|-------------|
| **PCAP** | libpcap/npcap packet capture | Real network access, most portable | Yes |
| **TAP** | TUN/TAP kernel driver | Linux/BSD virtual networking | Yes |
| **VDE** | Virtual Distributed Ethernet | Multi-simulator networking | Yes |
| **UDP** | Raw UDP socket transport | Simulator-to-simulator over IP | Yes |
| **NAT** | libslirp userspace TCP/IP | Guest internet access, no root needed | **NO** (mutex required) |
| **TEST** | In-memory test harness | Unit testing, CI | Yes |

## Important Implementation Details

### Lock-Free Queues

The `read_queue` and `write_requests` use `sim_tailq_t`, a **Single-Producer Single-Consumer (SPSC)** lock-free queue:
- Reader thread is sole producer for `read_queue`
- Main thread is sole consumer for `read_queue`
- Main thread is sole producer for `write_requests`
- Writer thread is sole consumer for `write_requests`

**No locks needed for enqueue/dequeue operations** - lock-free algorithm uses atomic operations internally.

### Write Buffer Management

The writer thread implements **batch freelist returns** to minimize lock contention:
1. Writer dequeues all pending requests
2. Processes each packet
3. Accumulates freed buffers in local linked list
4. **One lock acquisition** returns entire batch to global freelist

This reduces lock operations from O(n) to O(1) per write batch.

### Throttling

Configurable transmit throttling prevents overwhelming slow backends:
```c
if (dev->throttle_delay != ETH_THROT_DISABLED_DELAY) {
    uint32_t packet_delta_time = sim_os_msec() - dev->throttle_packet_time;
    dev->throttle_events <<= 1;
    dev->throttle_events += (packet_delta_time < dev->throttle_time) ? 1 : 0;
    
    if ((dev->throttle_events & dev->throttle_mask) == dev->throttle_mask) {
        sim_os_ms_sleep(dev->throttle_delay);
        ++dev->throttle_count;
    }
}
```

Tracks inter-packet timing with shift register to detect bursts.

### IP Checksum Offload Handling

Modern NICs offload IP/TCP/UDP checksum computation. When packets loop back through virtual networks without traversing real hardware, checksums may be zero or incorrect.

`eth_fix_ip_xsum_offload()` detects locally-originated packets (source MAC = host NIC) and recomputes:
- IP header checksum
- TCP/UDP pseudo-header checksums
- ICMP checksums

This ensures simulated systems receive valid checksums even when the host NIC didn't compute them.

### Jumbo Frame Handling

Frames larger than 1514 bytes (jumbo frames) must be fragmented for delivery to virtual NICs with standard MTU:

**For UDP/ICMP**: IP fragmentation (DF flag cleared, offset field used)
**For TCP**: TCP segmentation (multiple TCP segments with adjusted sequence numbers)

See `eth_fix_ip_jumbo_offload()` for the complete algorithm.

### CRC Generation

Ethernet CRC-32 is optional depending on the backend and simulated device requirements:
- Some backends (PCAP) handle CRC automatically
- Others require software CRC generation
- `eth_get_packet_crc32_data()` computes CRC-32 using the Ethernet polynomial
- Stored separately in queue item to avoid modifying received data

## Debugging

Enable debug output via SIMH's debug framework:
```
SET <device> DEBUG=ETHERNET        ; General ethernet events
SET <device> DEBUG=DATA            ; Packet contents
SET <device> DEBUG=POLL            ; libslirp polling (NAT backend)
SET <device> DEBUG=SOCKET          ; libslirp socket activity
```

**Packet Tracing**: `eth_packet_trace()` and `eth_packet_trace_ex()` log packet direction and hex dumps.

## Thread Affinity

Both reader and writer threads are assigned to the **I/O CPU partition** via `sim_os_set_thread_affinity()`. This separates network I/O from emulation cores on NUMA systems, reducing cache contention.

## Porting Notes

### Adding a New Backend

1. Define new `ETH_API_xxx` in `eth_types.h`
2. Add state to `eth_backend_t.state` union in `eth_backends.h`
3. Implement functions in new source file:
   - `int eth_wait_xxx(backend, dev)` - wait for packet arrival
   - `int eth_reader_xxx(backend, dev)` - read and queue packet
   - `int eth_writer_xxx(dev, packet)` - write one packet
   - Optional: before/after write hooks, shutdown hooks
4. Add entries to dispatch tables in `eth_dispatch.c`
5. Add open/close functions for device setup

### Platform Considerations

**Windows**:
- Uses `WaitForSingleObject()` instead of select/poll for PCAP
- BPF definitions conflict between `net/bpf.h` and `pcap/bpf.h` - include order matters
- Socket type is `SOCKET` (unsigned), not `int`

**macOS**:
- BPF must be included before PCAP headers (macOS 15.5+)
- TAP devices require explicit `IFF_UP` via `SIOCSIFFLAGS`

**Linux**:
- Uses `AF_PACKET` for device enumeration
- TUN/TAP via `/dev/net/tun` with `IFF_NO_PI` flag

**BSD**:
- Uses `AF_LINK` for device enumeration  
- Direct TAP device open: `/dev/tapN`
- VDE support via libvdeplug

## Future Maintainers: Critical Points

1. **libslirp is NOT thread-safe**: Always acquire `libslirp_lock` before any libslirp call. The before/after write hooks exist specifically for this.

2. **Lock-free queues are SPSC only**: Do not add multiple producers or consumers. If you need that, replace `sim_tailq_t` with a proper MPMC queue.

3. **Packet data ownership**: 
   - `eth_process_received_packet()` receives a pointer to backend-managed memory. It must copy data for queuing.
   - Oversized packets require heap allocation (`packet.oversize`). Always free in `ethq_item_free()`.

4. **CRC handling**: Different backends have different CRC expectations. PCAP handles CRC, TAP/VDE expect no CRC, some simulated NICs require CRC. Test thoroughly when modifying.

5. **Checksum offload**: When adding a new backend that doesn't traverse real hardware, ensure `eth_fix_ip_xsum_offload()` is called, otherwise TCP connections may fail mysteriously.

6. **Reader thread early exit**: If the reader thread returns with `ETH_READER_ERROR`, the device is dead. Ensure proper cleanup in the device detach routine.

7. **Dispatch table size**: `eth_reader_dispatch_table` is sized by `ETH_API_COUNT`. When adding a new backend, ensure the enum and table stay in sync.

## Testing

**Test Backend** (`ETH_API_TEST`):
- In-memory packet queues (`ETH_TEST_BACKEND`)
- No actual network I/O
- Useful for unit tests and CI environments
- Inject packets via `tx_from_guest`, verify via `rx_to_guest`

## Performance Characteristics

- **Lock contention points**: 
  - Writer freelist (batch-optimized to O(1) per batch)
  - libslirp mutex (unavoidable, libslirp limitation)
  - Startup synchronization (one-time cost)

- **Memory allocation**:
  - Packet items: allocated per-packet on enqueue, freed on dequeue
  - Write buffers: recycled freelist, pre-allocated pool
  - Jumbo frames: heap allocation for oversize buffer

- **CPU affinity**: Threads pinned to I/O cores, improves cache locality on NUMA

## Related Files

- `sim_ether.h` - Public ethernet device API
- `sim_ether_internal.h` - Internal API (not included in this directory)
- `sim_tailq.h` - Lock-free SPSC queue implementation
- `sim_threads.h` - Cross-platform threading primitives
- `poll_compat.h` - poll()/select() abstraction layer

## License

Files are dual-licensed:
- SPDX-FileCopyrightText: 2026 The ZIMH Project (newer files)
- SPDX-FileCopyrightText: 2002-2005 David T. Hittner (original eth_pktreader.c)

See individual file headers for specific licenses (MIT, X11).

---

**Last Updated**: 2026-09-01  
**Maintainer**: ZIMH Project  
**Contact**: See project documentation for details

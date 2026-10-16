# TetherLite

A userspace RNDIS driver for macOS, in C++20. It turns an RNDIS device (an Android phone with USB tethering, an embedded Linux gadget) into a network interface macOS can see, with no kext, no DriverKit and no SIP changes.

Modeled on [XiaoMiku01/TetherKit](https://github.com/XiaoMiku01/TetherKit): same approach (libusb for the USB side, a `feth` virtual interface pair plus BPF for the network side), written from scratch.

> **Status: v0.1 scaffold, not yet run on hardware.** The protocol codec is unit-tested (ASan/UBSan clean). The USB, `feth` and BPF layers were written against documented APIs and XNU source but have **not** been compiled on macOS or tried with a real device. Expect to fix small things on first build. Throughput is untested.

## How it works

```
 IP stack / DHCP / routes
        |
     feth0  <-- peered pair -->  feth1
   (system side)              (driver side)
                                  | BPF read/write of raw Ethernet frames
                              TetherLite
              TX: BPF -> wrap in RNDIS_PACKET_MSG -> bulk OUT
              RX: bulk IN -> split RNDIS_PACKET_MSG -> BPF write
              control: INITIALIZE, SET packet filter, KEEPALIVE, HALT
                                  | libusb
                             RNDIS device
```

| File | Role |
|---|---|
| `include/tetherlite/rndis.hpp`, `src/rndis.cpp` | Wire-format builders and bounds-checked parsers. No I/O. |
| `src/usb_device.*` | libusb: finds the RNDIS control and data interfaces, CDC encapsulated commands, bulk and interrupt I/O. |
| `src/feth.*` | Creates and peers a `feth` pair via private ioctls. |
| `src/bpf.*` | `/dev/bpfN` read and write. |
| `src/bridge.*` | Handshake, keepalive thread, TX and RX threads, stats, shutdown. |
| `tests/test_rndis.cpp` | Codec tests, including malformed-input cases. |

## Build

macOS 13+, Xcode command line tools, CMake 3.24+, libusb:

```sh
brew install libusb cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On other platforms only the codec and its tests are built.

## Run

```sh
./build/tetherlite --list                 # no root needed
sudo sysctl -w net.link.fake.hwcsum=0     # feth snapshots this at creation (per TetherKit's docs)
sudo ./build/tetherlite --stats 1000
# in another terminal:
sudo ipconfig set feth0 DHCP
ipconfig getifaddr feth0
```

Root is needed for the network side (creating `feth`, opening `/dev/bpf*`), not for USB. `ipconfig set` services are temporary and disappear on the next network reconfiguration.

## Known limitations (v0.1)

- One Ethernet frame per RNDIS message in both directions. No aggregation, so throughput will be well below what the hardware can do.
- Synchronous libusb transfers on dedicated threads. No async transfer pool, no lock-free queues.
- Only the active USB configuration is probed for RNDIS interfaces.
- `INDICATE_STATUS` (link up/down) is ignored; no automatic reset/reconnect.
- `feth` is a private XNU interface (struct layouts, ioctl numbers, command codes). It can change between macOS releases; `src/feth.cpp` documents what it assumes.
- The device MAC is not queried or applied to the system-side interface.

## Next steps

1. Build on a Mac and run against one device, with `--verbose`.
2. Add `OID_802_3_PERMANENT_ADDRESS` query and apply it to `feth0`.
3. Aggregate multiple frames per bulk OUT up to the device's `MaxPacketsPerMessage`.
4. Move to async libusb transfers and measure.
5. Handle `INDICATE_STATUS` and device reset.

MIT licensed (add a `LICENSE` file before publishing).

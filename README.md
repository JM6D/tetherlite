I’ve built TetherLite, a macOS userspace RNDIS driver (no kext, no DriverKit, no SIP changes) using the same approach as TetherKit. This is an AI job using Claude code.

It’s packaged as tetherlite.zip. How it works:

USB side: libusb finds the RNDIS device, does the RNDIS handshake and sets the packet filter, and answers keepalives.
Network side: a peered pair of feth virtual interfaces (feth0 for the system, feth1 for the driver). BPF reads and writes raw Ethernet frames on feth1.
Data path: two threads, TX and RX. TX sends each frame as one RNDIS packet message over USB. RX unpacks incoming messages and injects the frames through BPF.

The project is C++20 with a CMake build, a CLI (--list, --vid, --pid, --stats, --verbose), unit tests and a README. I used C++20 rather than TetherKit’s C++23 so it builds on older Apple clang versions.

What Claude verified and what it didn’t:

Verified: the protocol codec (rndis.cpp) builds and its tests pass under ASan and UBSan, including malformed-input cases. bridge.cpp and main.cpp pass a syntax check.
Not verified: usb_device.cpp, feth.cpp and bpf.cpp have not been compiled at all, since this sandbox has no libusb or macOS headers. Claude also couldn’t run the CMake build here (cmake isn’t installed), so it compiled the tests directly with g++.
Risky part: feth is a private XNU interface. Claude wrote the ioctl numbers and struct layouts from my recollection of the XNU source, so check src/feth.cpp first if creating the pair fails.
Performance: untested, and it will be slow by design. It sends one frame per bulk transfer, with no aggregation and no async transfers. The README lists the next steps.

Took me 20 minutes to reach the end on a new hachintosh install (Tahoe)

To try it on a Mac:

Sh
brew install libusb cmake
cmake -S . -B build && cmake --build build -j
./build/tetherlite --list
sudo sysctl -w net.link.fake.hwcsum=0   # this setting comes from TetherKit's docs
sudo ./build/tetherlite --verbose
sudo ipconfig set feth0 DHCP

If you want to create a new project on yourself tell to Claude which RNDIS device you’ll test with, Claude can tailor the next iteration (for example MAC handling or frame aggregation). If the first build fails, send me the compiler errors.





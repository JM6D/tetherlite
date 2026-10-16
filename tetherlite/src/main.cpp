#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>

#include "bridge.hpp"
#include "usb_device.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

void usage() {
  std::puts(
      "tetherlite - userspace RNDIS driver for macOS\n"
      "\n"
      "  tetherlite --list                 list RNDIS devices (no root needed)\n"
      "  sudo tetherlite [options]         bridge the first RNDIS device to a feth interface\n"
      "\n"
      "options:\n"
      "  --vid HEX --pid HEX   select a specific device\n"
      "  --stats MS            print throughput counters every MS milliseconds\n"
      "  --verbose             protocol details\n"
      "  -h, --help");
}

std::optional<uint16_t> parse_hex(const char* s) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 16);
  if (end == s || *end || v > 0xFFFF) return std::nullopt;
  return static_cast<uint16_t>(v);
}

}  // namespace

int main(int argc, char** argv) {
  bool list = false;
  std::optional<uint16_t> vid, pid;
  tl::BridgeOptions opt;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
      return argv[++i];
    };
    if (a == "--list") list = true;
    else if (a == "--vid") { vid = parse_hex(value()); if (!vid) { std::fputs("bad --vid\n", stderr); return 2; } }
    else if (a == "--pid") { pid = parse_hex(value()); if (!pid) { std::fputs("bad --pid\n", stderr); return 2; } }
    else if (a == "--stats") opt.stats_interval_ms = static_cast<unsigned>(std::strtoul(value(), nullptr, 10));
    else if (a == "--verbose") opt.verbose = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
  }

  try {
    if (list) {
      const auto devs = tl::UsbRndisDevice::list();
      if (devs.empty()) std::puts("no RNDIS devices found");
      for (const auto& d : devs) std::printf("%04x:%04x  bus %u addr %u\n", d.vid, d.pid, d.bus, d.address);
      return 0;
    }
    if (geteuid() != 0) {
      std::fputs("root is required to create feth interfaces and open /dev/bpf*; use sudo\n", stderr);
      return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    auto dev = tl::UsbRndisDevice::open(vid, pid);
    std::fprintf(stderr, "Opened %04x:%04x\n", dev->info().vid, dev->info().pid);
    return tl::run_bridge(*dev, opt, g_stop);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

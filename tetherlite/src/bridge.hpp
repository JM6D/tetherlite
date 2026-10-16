#pragma once
#include <atomic>
#include <optional>

#include "usb_device.hpp"

namespace tl {

struct BridgeOptions {
  unsigned stats_interval_ms = 0;  // 0 = no periodic stats
  bool verbose = false;
};

// Runs until `stop` becomes true or the device disappears. Returns a process exit code.
int run_bridge(UsbRndisDevice& dev, const BridgeOptions& opt, const std::atomic<bool>& stop);

}  // namespace tl

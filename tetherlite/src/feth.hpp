#pragma once
#include <string>

namespace tl {

// A pair of peered feth (if_fake) interfaces. The "system" side is what the IP stack,
// DHCP client and routing table use; the "driver" side is what we read/write via BPF.
// Requires root. Both interfaces are destroyed in the destructor.
class FethPair {
 public:
  static FethPair create();
  ~FethPair();
  FethPair(FethPair&& o) noexcept;
  FethPair& operator=(FethPair&&) = delete;
  FethPair(const FethPair&) = delete;

  const std::string& system_name() const { return sys_; }
  const std::string& driver_name() const { return drv_; }

 private:
  FethPair() = default;
  std::string sys_, drv_;
};

}  // namespace tl

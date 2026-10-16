#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tl {

// Raw Ethernet access to one interface through /dev/bpfN. Requires root.
class Bpf {
 public:
  static Bpf open(const std::string& ifname);
  ~Bpf();
  Bpf(Bpf&& o) noexcept;
  Bpf(const Bpf&) = delete;
  Bpf& operator=(const Bpf&) = delete;

  // Blocks up to ~200 ms. Calls `fn` once per captured frame; returns the number of frames.
  size_t read_frames(const std::function<void(std::span<const uint8_t>)>& fn);
  // Injects one Ethernet frame (source MAC is preserved).
  bool write(std::span<const uint8_t> frame);

 private:
  Bpf() = default;
  int fd_ = -1;
  std::vector<uint8_t> buf_;
};

}  // namespace tl

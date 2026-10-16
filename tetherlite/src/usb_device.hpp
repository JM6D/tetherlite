#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tetherlite/rndis.hpp"

struct libusb_context;
struct libusb_device_handle;

namespace tl {

struct UsbInfo {
  uint16_t vid = 0, pid = 0;
  uint8_t bus = 0, address = 0;
};

// Thin libusb wrapper for a CDC-style RNDIS device: one control interface (encapsulated
// commands + interrupt notifications) and one data interface (bulk IN/OUT).
class UsbRndisDevice {
 public:
  static std::vector<UsbInfo> list();
  // Opens the first matching RNDIS device. Throws std::runtime_error with a readable message.
  static std::unique_ptr<UsbRndisDevice> open(std::optional<uint16_t> vid, std::optional<uint16_t> pid);
  ~UsbRndisDevice();
  UsbRndisDevice(const UsbRndisDevice&) = delete;
  UsbRndisDevice& operator=(const UsbRndisDevice&) = delete;

  const UsbInfo& info() const { return info_; }
  uint16_t bulk_out_max_packet() const { return max_pkt_out_; }

  // Control channel (CDC SEND/GET_ENCAPSULATED_*).
  void send_command(rndis::View msg);
  std::optional<rndis::Bytes> get_response(unsigned timeout_ms);
  bool wait_notification(unsigned timeout_ms);  // interrupt IN; true if one arrived

  // Data channel. nullopt means timeout. Fatal errors (unplug, I/O) throw.
  std::optional<size_t> bulk_in(uint8_t* buf, size_t cap, unsigned timeout_ms);
  void bulk_out(rndis::View data, unsigned timeout_ms = 1000);

 private:
  UsbRndisDevice() = default;
  libusb_context* ctx_ = nullptr;
  libusb_device_handle* h_ = nullptr;
  UsbInfo info_;
  int ctrl_if_ = -1, data_if_ = -1, data_alt_ = 0;
  uint8_t ep_in_ = 0, ep_out_ = 0, ep_int_ = 0;
  uint16_t max_pkt_out_ = 512;
  bool ctrl_claimed_ = false, data_claimed_ = false;
};

}  // namespace tl

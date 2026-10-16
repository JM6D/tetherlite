#include "usb_device.hpp"

#include <libusb.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace tl {
namespace {

struct Layout {
  int ctrl_if = -1, data_if = -1, data_alt = 0;
  uint8_t ep_in = 0, ep_out = 0, ep_int = 0;
  uint16_t max_pkt_out = 512;
};

std::runtime_error usb_error(const char* what, int rc) {
  return std::runtime_error(std::string(what) + " [libusb: " + libusb_error_name(rc) + "]");
}

bool is_rndis_control(const libusb_interface_descriptor& a) {
  return (a.bInterfaceClass == 0xE0 && a.bInterfaceSubClass == 0x01 && a.bInterfaceProtocol == 0x03) ||
         (a.bInterfaceClass == 0x02 && a.bInterfaceSubClass == 0x02 && a.bInterfaceProtocol == 0xFF);
}

// Looks for an RNDIS control interface plus a bulk data interface in the *active* configuration.
std::optional<Layout> probe(libusb_device* dev) {
  libusb_config_descriptor* cfg = nullptr;
  if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) return std::nullopt;
  std::optional<Layout> result;

  for (int i = 0; i < cfg->bNumInterfaces && !result; ++i) {
    const libusb_interface& itf = cfg->interface[i];
    if (itf.num_altsetting < 1 || !is_rndis_control(itf.altsetting[0])) continue;

    Layout l;
    l.ctrl_if = itf.altsetting[0].bInterfaceNumber;
    for (int e = 0; e < itf.altsetting[0].bNumEndpoints; ++e) {
      const auto& ep = itf.altsetting[0].endpoint[e];
      if ((ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_INTERRUPT && (ep.bEndpointAddress & 0x80))
        l.ep_int = ep.bEndpointAddress;
    }

    for (int j = 0; j < cfg->bNumInterfaces; ++j) {
      const libusb_interface& d = cfg->interface[j];
      for (int k = 0; k < d.num_altsetting; ++k) {
        const auto& alt = d.altsetting[k];
        if (alt.bInterfaceClass != 0x0A) continue;  // CDC Data
        uint8_t in = 0, out = 0;
        uint16_t mps = 512;
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
          const auto& ep = alt.endpoint[e];
          if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
          if (ep.bEndpointAddress & 0x80) in = ep.bEndpointAddress;
          else { out = ep.bEndpointAddress; mps = ep.wMaxPacketSize; }
        }
        if (in && out && !l.ep_in) {
          l.data_if = alt.bInterfaceNumber;
          l.data_alt = alt.bAlternateSetting;
          l.ep_in = in;
          l.ep_out = out;
          l.max_pkt_out = mps;
        }
      }
    }
    if (l.ep_in && l.ep_out) result = l;
  }
  libusb_free_config_descriptor(cfg);
  return result;
}

}  // namespace

std::vector<UsbInfo> UsbRndisDevice::list() {
  std::vector<UsbInfo> out;
  libusb_context* ctx = nullptr;
  if (libusb_init(&ctx) != 0) return out;
  libusb_device** devs = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &devs);
  for (ssize_t i = 0; i < n; ++i) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(devs[i], &dd) != 0 || !probe(devs[i])) continue;
    out.push_back({dd.idVendor, dd.idProduct, libusb_get_bus_number(devs[i]), libusb_get_device_address(devs[i])});
  }
  if (n >= 0) libusb_free_device_list(devs, 1);
  libusb_exit(ctx);
  return out;
}

std::unique_ptr<UsbRndisDevice> UsbRndisDevice::open(std::optional<uint16_t> vid, std::optional<uint16_t> pid) {
  std::unique_ptr<UsbRndisDevice> d(new UsbRndisDevice());
  if (int rc = libusb_init(&d->ctx_); rc != 0) throw usb_error("libusb_init failed", rc);

  libusb_device** devs = nullptr;
  const ssize_t n = libusb_get_device_list(d->ctx_, &devs);
  if (n < 0) throw usb_error("cannot enumerate USB devices", static_cast<int>(n));

  std::string last_error = "no RNDIS device found (enable USB tethering on the device and use a data cable)";
  bool found = false;
  for (ssize_t i = 0; i < n && !found; ++i) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(devs[i], &dd) != 0) continue;
    if ((vid && *vid != dd.idVendor) || (pid && *pid != dd.idProduct)) continue;
    auto layout = probe(devs[i]);
    if (!layout) continue;
    if (int rc = libusb_open(devs[i], &d->h_); rc != 0) {
      last_error = std::string("cannot open device [libusb: ") + libusb_error_name(rc) + "]";
      d->h_ = nullptr;
      continue;
    }
    d->info_ = {dd.idVendor, dd.idProduct, libusb_get_bus_number(devs[i]), libusb_get_device_address(devs[i])};
    d->ctrl_if_ = layout->ctrl_if;
    d->data_if_ = layout->data_if;
    d->data_alt_ = layout->data_alt;
    d->ep_in_ = layout->ep_in;
    d->ep_out_ = layout->ep_out;
    d->ep_int_ = layout->ep_int;
    d->max_pkt_out_ = layout->max_pkt_out;
    found = true;
  }
  libusb_free_device_list(devs, 1);
  if (!found) throw std::runtime_error(last_error);

  (void)libusb_set_auto_detach_kernel_driver(d->h_, 1);  // unsupported on macOS; harmless
  if (int rc = libusb_claim_interface(d->h_, d->ctrl_if_); rc != 0) throw usb_error("cannot claim RNDIS control interface", rc);
  d->ctrl_claimed_ = true;
  if (d->data_if_ != d->ctrl_if_) {
    if (int rc = libusb_claim_interface(d->h_, d->data_if_); rc != 0) throw usb_error("cannot claim RNDIS data interface", rc);
    d->data_claimed_ = true;
  }
  if (d->data_alt_ != 0) {
    if (int rc = libusb_set_interface_alt_setting(d->h_, d->data_if_, d->data_alt_); rc != 0)
      throw usb_error("cannot select data alt setting", rc);
  }
  return d;
}

UsbRndisDevice::~UsbRndisDevice() {
  if (h_) {
    if (data_claimed_) libusb_release_interface(h_, data_if_);
    if (ctrl_claimed_) libusb_release_interface(h_, ctrl_if_);
    libusb_close(h_);
  }
  if (ctx_) libusb_exit(ctx_);
}

void UsbRndisDevice::send_command(rndis::View msg) {
  // CDC SEND_ENCAPSULATED_COMMAND: bmRequestType 0x21, bRequest 0x00.
  const int r = libusb_control_transfer(h_, 0x21, 0x00, 0, static_cast<uint16_t>(ctrl_if_),
                                        const_cast<uint8_t*>(msg.data()), static_cast<uint16_t>(msg.size()), 1000);
  if (r < 0) throw usb_error("SEND_ENCAPSULATED_COMMAND failed", r);
}

std::optional<rndis::Bytes> UsbRndisDevice::get_response(unsigned timeout_ms) {
  // CDC GET_ENCAPSULATED_RESPONSE: bmRequestType 0xA1, bRequest 0x01.
  rndis::Bytes buf(4096);
  const int r = libusb_control_transfer(h_, 0xA1, 0x01, 0, static_cast<uint16_t>(ctrl_if_), buf.data(),
                                        static_cast<uint16_t>(buf.size()), timeout_ms);
  if (r == LIBUSB_ERROR_TIMEOUT || r == LIBUSB_ERROR_PIPE || r == 0) return std::nullopt;  // nothing queued
  if (r < 0) throw usb_error("GET_ENCAPSULATED_RESPONSE failed", r);
  buf.resize(static_cast<size_t>(r));
  return buf;
}

bool UsbRndisDevice::wait_notification(unsigned timeout_ms) {
  if (!ep_int_) {  // no interrupt endpoint: just pace the caller
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return false;
  }
  uint8_t b[16];
  int got = 0;
  const int r = libusb_interrupt_transfer(h_, ep_int_, b, sizeof b, &got, timeout_ms);
  if (r == 0) return true;
  if (r == LIBUSB_ERROR_TIMEOUT) return false;
  throw usb_error("interrupt read failed", r);
}

std::optional<size_t> UsbRndisDevice::bulk_in(uint8_t* buf, size_t cap, unsigned timeout_ms) {
  int got = 0;
  const int r = libusb_bulk_transfer(h_, ep_in_, buf, static_cast<int>(cap), &got, timeout_ms);
  if (r == 0) return static_cast<size_t>(got);
  if (r == LIBUSB_ERROR_TIMEOUT) return std::nullopt;
  throw usb_error("bulk IN failed", r);
}

void UsbRndisDevice::bulk_out(rndis::View data, unsigned timeout_ms) {
  int sent = 0;
  const int r = libusb_bulk_transfer(h_, ep_out_, const_cast<uint8_t*>(data.data()), static_cast<int>(data.size()),
                                     &sent, timeout_ms);
  if (r != 0) throw usb_error("bulk OUT failed", r);
}

}  // namespace tl

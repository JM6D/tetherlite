#include "bridge.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>

#include "bpf.hpp"
#include "feth.hpp"
#include "tetherlite/rndis.hpp"

namespace tl {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kHostMaxTransfer = 16384;  // max device->host message we accept

struct Stats {
  std::atomic<uint64_t> tx_frames{0}, tx_bytes{0}, rx_frames{0}, rx_bytes{0}, drops{0};
};

uint32_t next_id() {
  static std::atomic<uint32_t> id{1};
  return id++;
}

// Answers device-initiated messages (keepalive); returns true if the message was one of those.
bool handle_unsolicited(UsbRndisDevice& dev, rndis::View msg) {
  auto h = rndis::peek_header(msg);
  if (!h) return true;  // junk: drop
  if (h->type == rndis::kKeepalive) {
    dev.send_command(rndis::make_keepalive_complete(rndis::rd32(msg, 8)));
    return true;
  }
  return h->type == rndis::kIndicateStatus;  // media connect/disconnect: ignored in this version
}

rndis::Bytes transact(UsbRndisDevice& dev, rndis::View request, uint32_t req_id) {
  dev.send_command(request);
  const auto deadline = Clock::now() + 3s;
  while (Clock::now() < deadline) {
    auto r = dev.get_response(100);
    if (!r) {
      dev.wait_notification(50);
      continue;
    }
    if (handle_unsolicited(dev, *r)) continue;
    if (r->size() >= 12 && rndis::rd32(*r, 8) == req_id) return std::move(*r);
  }
  throw std::runtime_error("RNDIS device did not answer in time");
}

rndis::InitComplete handshake(UsbRndisDevice& dev, bool verbose) {
  const uint32_t id = next_id();
  auto resp = transact(dev, rndis::make_initialize(id, kHostMaxTransfer), id);
  auto init = rndis::parse_init_complete(resp);
  if (!init) throw std::runtime_error("malformed RNDIS INITIALIZE_CMPLT");
  if (init->status != rndis::kStatusSuccess) throw std::runtime_error("device rejected RNDIS INITIALIZE");
  if (verbose)
    std::fprintf(stderr, "[rndis] max_transfer=%u max_packets=%u align=%u\n", init->max_transfer_size,
                 init->max_packets_per_message, init->packet_alignment);

  // Accept directed, multicast, broadcast.
  const uint32_t fid = next_id();
  const uint32_t filter = rndis::kFilterDirected | rndis::kFilterMulticast | rndis::kFilterBroadcast;
  auto fresp = transact(dev, rndis::make_set_u32(fid, rndis::kOidCurrentPacketFilter, filter), fid);
  auto fc = rndis::parse_completion(fresp);
  if (!fc || fc->status != rndis::kStatusSuccess) throw std::runtime_error("device rejected packet filter");
  return *init;
}

}  // namespace

int run_bridge(UsbRndisDevice& dev, const BridgeOptions& opt, const std::atomic<bool>& stop) {
  const auto init = handshake(dev, opt.verbose);
  const size_t max_out = init.max_transfer_size ? init.max_transfer_size : 16384;

  // Declaration order matters: Bpf closes before the feth pair is destroyed.
  FethPair pair = FethPair::create();
  Bpf bpf = Bpf::open(pair.driver_name());
  std::fprintf(stderr, "Created %s (system side) <-> %s (driver side)\n", pair.system_name().c_str(),
               pair.driver_name().c_str());
  std::fprintf(stderr, "Next:  sudo ipconfig set %s DHCP && ipconfig getifaddr %s\n", pair.system_name().c_str(),
               pair.system_name().c_str());

  Stats st;
  std::atomic<bool> fatal{false};
  auto running = [&] { return !stop.load() && !fatal.load(); };
  auto guarded = [&](const char* name, auto&& body) {
    return std::thread([&, name, body] {
      try { body(); }
      catch (const std::exception& e) {
        std::fprintf(stderr, "[%s] %s\n", name, e.what());
        fatal = true;
      }
    });
  };

  // Control: answers keepalives and drains notifications.
  auto ctrl = guarded("control", [&] {
    while (running()) {
      if (dev.wait_notification(500))
        while (auto r = dev.get_response(20)) handle_unsolicited(dev, *r);
    }
  });

  // USB -> macOS
  auto rx = guarded("rx", [&] {
    rndis::Bytes buf(kHostMaxTransfer);
    while (running()) {
      auto n = dev.bulk_in(buf.data(), buf.size(), 200);
      if (!n || *n == 0) continue;
      for (rndis::View f : rndis::extract_packets(rndis::View(buf.data(), *n))) {
        if (bpf.write(f)) { ++st.rx_frames; st.rx_bytes += f.size(); }
        else ++st.drops;
      }
    }
  });

  // macOS -> USB (one frame per RNDIS message in this version; no aggregation)
  auto tx = guarded("tx", [&] {
    while (running()) {
      bpf.read_frames([&](std::span<const uint8_t> f) {
        auto msg = rndis::wrap_packet(f);
        if (msg.size() > max_out) { ++st.drops; return; }
        dev.bulk_out(msg);
        // A transfer that is an exact multiple of the endpoint packet size needs a zero-length packet to terminate.
        if (msg.size() % dev.bulk_out_max_packet() == 0) dev.bulk_out({});
        ++st.tx_frames;
        st.tx_bytes += f.size();
      });
    }
  });

  auto last = Clock::now();
  while (running()) {
    std::this_thread::sleep_for(100ms);
    if (opt.stats_interval_ms && Clock::now() - last >= std::chrono::milliseconds(opt.stats_interval_ms)) {
      last = Clock::now();
      std::fprintf(stderr, "tx %llu fr / %llu B   rx %llu fr / %llu B   drops %llu\n",
                   static_cast<unsigned long long>(st.tx_frames), static_cast<unsigned long long>(st.tx_bytes),
                   static_cast<unsigned long long>(st.rx_frames), static_cast<unsigned long long>(st.rx_bytes),
                   static_cast<unsigned long long>(st.drops));
    }
  }

  fatal = true;  // make every worker exit
  ctrl.join(); rx.join(); tx.join();

  try { dev.send_command(rndis::make_halt(next_id())); } catch (...) {}  // best effort; device may be gone
  return stop.load() ? 0 : 1;
}

}  // namespace tl

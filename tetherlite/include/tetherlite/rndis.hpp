// Pure RNDIS wire-format codec. No I/O, no platform dependencies, fully unit-testable.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tl::rndis {

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

// Message types. Completions have the high bit set.
inline constexpr uint32_t kPacket = 0x00000001;
inline constexpr uint32_t kInitialize = 0x00000002;
inline constexpr uint32_t kHalt = 0x00000003;
inline constexpr uint32_t kQuery = 0x00000004;
inline constexpr uint32_t kSet = 0x00000005;
inline constexpr uint32_t kReset = 0x00000006;
inline constexpr uint32_t kIndicateStatus = 0x00000007;
inline constexpr uint32_t kKeepalive = 0x00000008;
inline constexpr uint32_t kCompleteBit = 0x80000000;

inline constexpr uint32_t kStatusSuccess = 0x00000000;

// OIDs used by the host side.
inline constexpr uint32_t kOidMaxFrameSize = 0x00010106;
inline constexpr uint32_t kOidLinkSpeed = 0x00010107;
inline constexpr uint32_t kOidCurrentPacketFilter = 0x0001010E;
inline constexpr uint32_t kOidMediaConnectStatus = 0x00010114;
inline constexpr uint32_t kOidPermanentAddress = 0x01010101;
inline constexpr uint32_t kOidCurrentAddress = 0x01010102;

// Packet filter bits.
inline constexpr uint32_t kFilterDirected = 0x01;
inline constexpr uint32_t kFilterMulticast = 0x02;
inline constexpr uint32_t kFilterAllMulticast = 0x04;
inline constexpr uint32_t kFilterBroadcast = 0x08;
inline constexpr uint32_t kFilterPromiscuous = 0x20;

inline constexpr size_t kPacketHeaderSize = 44;  // RNDIS_PACKET_MSG fixed header

// ---- little-endian helpers ----
uint32_t rd32(View b, size_t off);

// ---- builders (host -> device) ----
Bytes make_initialize(uint32_t req_id, uint32_t max_transfer);
Bytes make_halt(uint32_t req_id);
Bytes make_query(uint32_t req_id, uint32_t oid);
Bytes make_set(uint32_t req_id, uint32_t oid, View value);
Bytes make_set_u32(uint32_t req_id, uint32_t oid, uint32_t value);
Bytes make_keepalive_complete(uint32_t req_id);
Bytes wrap_packet(View ethernet_frame);

// ---- parsers (device -> host). Every parser bounds-checks and returns nullopt on malformed input ----
struct Header {
  uint32_t type;
  uint32_t length;
};
std::optional<Header> peek_header(View buf);

struct InitComplete {
  uint32_t req_id, status, major, minor, device_flags, medium;
  uint32_t max_packets_per_message, max_transfer_size, packet_alignment;
};
std::optional<InitComplete> parse_init_complete(View buf);

struct Completion {
  uint32_t type;    // e.g. kQuery | kCompleteBit
  uint32_t req_id;
  uint32_t status;
  View info;        // only populated for query completions
};
std::optional<Completion> parse_completion(View buf);

// Walks one bulk-IN transfer and returns every Ethernet frame inside it.
// Non-data messages are skipped; parsing stops at the first malformed message.
std::vector<View> extract_packets(View buf);

}  // namespace tl::rndis

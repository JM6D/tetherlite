#include "tetherlite/rndis.hpp"

#include <algorithm>

namespace tl::rndis {
namespace {

void put32(Bytes& v, size_t off, uint32_t x) {
  v[off] = static_cast<uint8_t>(x);
  v[off + 1] = static_cast<uint8_t>(x >> 8);
  v[off + 2] = static_cast<uint8_t>(x >> 16);
  v[off + 3] = static_cast<uint8_t>(x >> 24);
}

Bytes blank(uint32_t type, size_t len) {
  Bytes v(len, 0);
  put32(v, 0, type);
  put32(v, 4, static_cast<uint32_t>(len));
  return v;
}

Bytes make_info_msg(uint32_t type, uint32_t req_id, uint32_t oid, View value) {
  constexpr size_t kFixed = 28;
  Bytes v = blank(type, kFixed + value.size());
  put32(v, 8, req_id);
  put32(v, 12, oid);
  put32(v, 16, static_cast<uint32_t>(value.size()));
  put32(v, 20, value.empty() ? 0 : kFixed - 8);  // offset is measured from the RequestID field
  std::copy(value.begin(), value.end(), v.begin() + kFixed);
  return v;
}

}  // namespace

uint32_t rd32(View b, size_t off) {
  return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

Bytes make_initialize(uint32_t req_id, uint32_t max_transfer) {
  Bytes v = blank(kInitialize, 24);
  put32(v, 8, req_id);
  put32(v, 12, 1);  // major
  put32(v, 16, 0);  // minor
  put32(v, 20, max_transfer);
  return v;
}

Bytes make_halt(uint32_t req_id) {
  Bytes v = blank(kHalt, 12);
  put32(v, 8, req_id);
  return v;
}

Bytes make_query(uint32_t req_id, uint32_t oid) { return make_info_msg(kQuery, req_id, oid, {}); }

Bytes make_set(uint32_t req_id, uint32_t oid, View value) {
  return make_info_msg(kSet, req_id, oid, value);
}

Bytes make_set_u32(uint32_t req_id, uint32_t oid, uint32_t value) {
  const uint8_t b[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return make_set(req_id, oid, View(b, 4));
}

Bytes make_keepalive_complete(uint32_t req_id) {
  Bytes v = blank(kKeepalive | kCompleteBit, 16);
  put32(v, 8, req_id);
  put32(v, 12, kStatusSuccess);
  return v;
}

Bytes wrap_packet(View frame) {
  Bytes v = blank(kPacket, kPacketHeaderSize + frame.size());
  put32(v, 8, kPacketHeaderSize - 8);  // DataOffset, from the DataOffset field
  put32(v, 12, static_cast<uint32_t>(frame.size()));
  std::copy(frame.begin(), frame.end(), v.begin() + kPacketHeaderSize);
  return v;
}

std::optional<Header> peek_header(View buf) {
  if (buf.size() < 8) return std::nullopt;
  Header h{rd32(buf, 0), rd32(buf, 4)};
  if (h.length < 8 || h.length > buf.size()) return std::nullopt;
  return h;
}

std::optional<InitComplete> parse_init_complete(View buf) {
  auto h = peek_header(buf);
  if (!h || h->type != (kInitialize | kCompleteBit) || h->length < 52) return std::nullopt;
  return InitComplete{rd32(buf, 8),  rd32(buf, 12), rd32(buf, 16), rd32(buf, 20), rd32(buf, 24),
                      rd32(buf, 28), rd32(buf, 32), rd32(buf, 36), rd32(buf, 40)};
}

std::optional<Completion> parse_completion(View buf) {
  auto h = peek_header(buf);
  if (!h || !(h->type & kCompleteBit)) return std::nullopt;
  if (h->length < 16) return std::nullopt;
  Completion c{h->type, rd32(buf, 8), rd32(buf, 12), {}};
  if (h->type == (kQuery | kCompleteBit)) {
    if (h->length < 24) return std::nullopt;
    const uint64_t len = rd32(buf, 16);
    const uint64_t start = 8ull + rd32(buf, 20);
    if (len > 0) {
      if (start < 24 || start + len > h->length) return std::nullopt;
      c.info = buf.subspan(static_cast<size_t>(start), static_cast<size_t>(len));
    }
  }
  return c;
}

std::vector<View> extract_packets(View buf) {
  std::vector<View> out;
  size_t off = 0;
  while (buf.size() - off >= 8) {
    View rest = buf.subspan(off);
    const uint32_t type = rd32(rest, 0);
    const uint32_t len = rd32(rest, 4);
    if (len < 8 || len > rest.size()) break;
    if (type == kPacket && len >= kPacketHeaderSize) {
      const uint64_t start = 8ull + rd32(rest, 8);
      const uint64_t dlen = rd32(rest, 12);
      if (dlen > 0 && start >= kPacketHeaderSize && start + dlen <= len)
        out.push_back(rest.subspan(static_cast<size_t>(start), static_cast<size_t>(dlen)));
    }
    off += len;
  }
  return out;
}

}  // namespace tl::rndis

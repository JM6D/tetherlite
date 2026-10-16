#include <cstdio>
#include <cstdlib>

#include "tetherlite/rndis.hpp"

using namespace tl::rndis;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                      \
    }                                                                    \
  } while (0)

static void put32(Bytes& v, size_t off, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}

static void test_builders() {
  auto init = make_initialize(7, 16384);
  CHECK(init.size() == 24);
  CHECK(rd32(init, 0) == kInitialize && rd32(init, 4) == 24 && rd32(init, 8) == 7);
  CHECK(rd32(init, 12) == 1 && rd32(init, 16) == 0 && rd32(init, 20) == 16384);

  auto q = make_query(3, kOidPermanentAddress);
  CHECK(q.size() == 28 && rd32(q, 12) == kOidPermanentAddress && rd32(q, 16) == 0);

  auto s = make_set_u32(4, kOidCurrentPacketFilter, 0x0F);
  CHECK(s.size() == 32 && rd32(s, 16) == 4 && rd32(s, 20) == 20 && rd32(s, 28) == 0x0F);

  auto ka = make_keepalive_complete(9);
  CHECK(rd32(ka, 0) == (kKeepalive | kCompleteBit) && ka.size() == 16 && rd32(ka, 8) == 9);
}

static void test_packet_roundtrip() {
  Bytes f1(60, 0xAA), f2(1514, 0xBB);
  Bytes xfer;
  for (const Bytes* f : {&f1, &f2}) {
    auto m = wrap_packet(*f);
    CHECK(m.size() == 44 + f->size());
    xfer.insert(xfer.end(), m.begin(), m.end());
  }
  auto frames = extract_packets(xfer);
  CHECK(frames.size() == 2);
  CHECK(frames[0].size() == 60 && frames[0][0] == 0xAA);
  CHECK(frames[1].size() == 1514 && frames[1][1513] == 0xBB);
}

static void test_malformed_packets() {
  auto m = wrap_packet(Bytes(100, 1));
  // Truncated transfer: length field claims more than we have.
  CHECK(extract_packets(View(m.data(), 50)).empty());
  // DataLength overruns the message.
  Bytes bad = m;
  put32(bad, 12, 5000);
  CHECK(extract_packets(bad).empty());
  // DataOffset points back into the header.
  bad = m;
  put32(bad, 8, 4);
  CHECK(extract_packets(bad).empty());
  // Zero-length message would loop forever if not rejected.
  Bytes z(16, 0);
  put32(z, 0, kPacket);
  CHECK(extract_packets(z).empty());
  // Garbage after a good message does not discard the good one.
  Bytes mixed = m;
  mixed.insert(mixed.end(), {1, 2, 3});
  CHECK(extract_packets(mixed).size() == 1);
}

static void test_init_complete() {
  Bytes r(52, 0);
  put32(r, 0, kInitialize | kCompleteBit);
  put32(r, 4, 52);
  put32(r, 8, 1);       // req id
  put32(r, 12, 0);      // status
  put32(r, 32, 8);      // max packets per message
  put32(r, 36, 15796);  // max transfer
  put32(r, 40, 3);      // alignment
  auto c = parse_init_complete(r);
  CHECK(c && c->req_id == 1 && c->status == kStatusSuccess);
  CHECK(c->max_packets_per_message == 8 && c->max_transfer_size == 15796 && c->packet_alignment == 3);
  CHECK(!parse_init_complete(View(r.data(), 40)));
}

static void test_query_completion() {
  Bytes r(24 + 6, 0);
  put32(r, 0, kQuery | kCompleteBit);
  put32(r, 4, 30);
  put32(r, 8, 5);
  put32(r, 16, 6);   // info length
  put32(r, 20, 16);  // offset from RequestID field -> byte 24
  for (int i = 0; i < 6; ++i) r[24 + i] = static_cast<uint8_t>(0x10 + i);
  auto c = parse_completion(r);
  CHECK(c && c->req_id == 5 && c->info.size() == 6 && c->info[5] == 0x15);
  put32(r, 16, 60);  // info length overruns
  CHECK(!parse_completion(r));
}

int main() {
  test_builders();
  test_packet_roundtrip();
  test_malformed_packets();
  test_init_complete();
  test_query_completion();
  std::puts("all rndis tests passed");
}

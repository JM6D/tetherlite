// feth (if_fake) is a private XNU interface. The structs and constants below mirror
// xnu/bsd/net/if_fake.c and ifconfig's feth support. They are NOT a stable ABI:
// verify against your macOS version before trusting this file.
#include "feth.hpp"

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace tl {
namespace {

struct IfDrv {  // struct ifdrv, not exposed in the public SDK headers
  char ifd_name[IFNAMSIZ];
  unsigned long ifd_cmd;
  size_t ifd_len;
  void* ifd_data;
};

struct FakeRequest {  // struct if_fake_request
  uint64_t reserved[4];
  char buf[128];  // iffr_peer_name lives at the start of this union
};

constexpr unsigned long kIfCreate2 = _IOWR('i', 122, struct ifreq);   // SIOCIFCREATE2
constexpr unsigned long kIfDestroy = _IOW('i', 121, struct ifreq);    // SIOCIFDESTROY
constexpr unsigned long kSetDrvSpec = _IOW('i', 123, IfDrv);          // SIOCSDRVSPEC
constexpr unsigned long kCmdSetPeer = 1;                              // IF_FAKE_S_CMD_SET_PEER

class Sock {
 public:
  Sock() : fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {
    if (fd_ < 0) throw std::runtime_error("socket() failed");
  }
  ~Sock() { ::close(fd_); }
  int fd() const { return fd_; }
 private:
  int fd_;
};

ifreq named(const std::string& n) {
  ifreq r{};
  std::strlcpy(r.ifr_name, n.c_str(), sizeof r.ifr_name);
  return r;
}

[[noreturn]] void fail(const std::string& what) {
  if (errno == EPERM || errno == EACCES) throw std::runtime_error(what + ": needs root (run with sudo)");
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

bool create_if(Sock& s, const std::string& name) {
  ifreq r = named(name);
  if (::ioctl(s.fd(), kIfCreate2, &r) == 0) return true;
  if (errno == EEXIST) return false;
  fail("create " + name);
}

void destroy_if(const std::string& name) noexcept {
  try {
    Sock s;
    ifreq r = named(name);
    ::ioctl(s.fd(), kIfDestroy, &r);
  } catch (...) {}
}

void bring_up(Sock& s, const std::string& name) {
  ifreq r = named(name);
  if (::ioctl(s.fd(), SIOCGIFFLAGS, &r) < 0) fail("get flags " + name);
  r.ifr_flags |= IFF_UP;
  if (::ioctl(s.fd(), SIOCSIFFLAGS, &r) < 0) fail("bring up " + name);
}

// The fake interface snapshots these sysctls at creation time, so check before creating.
void check_sysctls() {
  int v = 0;
  size_t len = sizeof v;
  if (::sysctlbyname("net.link.fake.hwcsum", &v, &len, nullptr, 0) == 0 && v != 0)
    throw std::runtime_error(
        "net.link.fake.hwcsum is 1; run `sudo sysctl -w net.link.fake.hwcsum=0` and start again");
}

}  // namespace

FethPair FethPair::create() {
  check_sysctls();
  Sock s;
  FethPair p;
  for (int unit = 0; unit < 64; unit += 2) {
    const std::string a = "feth" + std::to_string(unit), b = "feth" + std::to_string(unit + 1);
    if (!create_if(s, a)) continue;
    p.sys_ = a;  // from here the destructor cleans up on any failure
    if (!create_if(s, b)) { destroy_if(a); p.sys_.clear(); continue; }
    p.drv_ = b;

    FakeRequest req{};
    std::strlcpy(req.buf, b.c_str(), sizeof req.buf);
    IfDrv d{};
    std::strlcpy(d.ifd_name, a.c_str(), sizeof d.ifd_name);
    d.ifd_cmd = kCmdSetPeer;
    d.ifd_len = sizeof req;
    d.ifd_data = &req;
    if (::ioctl(s.fd(), kSetDrvSpec, &d) < 0) fail("set feth peer");

    bring_up(s, a);
    bring_up(s, b);
    return p;
  }
  throw std::runtime_error("no free fethN pair found");
}

FethPair::FethPair(FethPair&& o) noexcept : sys_(std::move(o.sys_)), drv_(std::move(o.drv_)) {
  o.sys_.clear();
  o.drv_.clear();
}

FethPair::~FethPair() {
  if (!drv_.empty()) destroy_if(drv_);
  if (!sys_.empty()) destroy_if(sys_);
}

}  // namespace tl

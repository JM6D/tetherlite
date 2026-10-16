#include "bpf.hpp"

#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace tl {

Bpf Bpf::open(const std::string& ifname) {
  Bpf b;
  for (int i = 0; i < 256 && b.fd_ < 0; ++i) {
    const std::string path = "/dev/bpf" + std::to_string(i);
    b.fd_ = ::open(path.c_str(), O_RDWR);
    if (b.fd_ < 0 && errno != EBUSY) {
      if (errno == EACCES || errno == EPERM) throw std::runtime_error("cannot open " + path + ": needs root (run with sudo)");
      if (errno == ENOENT) break;  // ran out of nodes
    }
  }
  if (b.fd_ < 0) throw std::runtime_error("no free /dev/bpf device");

  u_int want = 512 * 1024;  // must be set before BIOCSETIF
  ::ioctl(b.fd_, BIOCSBLEN, &want);
  u_int blen = 0;
  if (::ioctl(b.fd_, BIOCGBLEN, &blen) < 0 || blen == 0) throw std::runtime_error("BIOCGBLEN failed");
  b.buf_.resize(blen);

  ifreq ifr{};
  std::strlcpy(ifr.ifr_name, ifname.c_str(), sizeof ifr.ifr_name);
  if (::ioctl(b.fd_, BIOCSETIF, &ifr) < 0) throw std::runtime_error("BIOCSETIF " + ifname + ": " + std::strerror(errno));

  u_int on = 1, off = 0;
  if (::ioctl(b.fd_, BIOCIMMEDIATE, &on) < 0) throw std::runtime_error("BIOCIMMEDIATE failed");
  if (::ioctl(b.fd_, BIOCSHDRCMPLT, &on) < 0) throw std::runtime_error("BIOCSHDRCMPLT failed");  // keep our source MAC on writes
  ::ioctl(b.fd_, BIOCSSEESENT, &off);                                                              // don't read back our own injections
  timeval tv{0, 200 * 1000};
  if (::ioctl(b.fd_, BIOCSRTIMEOUT, &tv) < 0) throw std::runtime_error("BIOCSRTIMEOUT failed");
  return b;
}

Bpf::Bpf(Bpf&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }

Bpf::~Bpf() {
  if (fd_ >= 0) ::close(fd_);
}

size_t Bpf::read_frames(const std::function<void(std::span<const uint8_t>)>& fn) {
  const ssize_t n = ::read(fd_, buf_.data(), buf_.size());
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    throw std::runtime_error(std::string("bpf read: ") + std::strerror(errno));
  }
  size_t frames = 0;
  const uint8_t* p = buf_.data();
  const uint8_t* end = p + n;
  while (static_cast<size_t>(end - p) >= sizeof(bpf_hdr)) {
    const auto* h = reinterpret_cast<const bpf_hdr*>(p);
    if (h->bh_hdrlen + h->bh_caplen > static_cast<size_t>(end - p)) break;
    fn(std::span<const uint8_t>(p + h->bh_hdrlen, h->bh_caplen));
    ++frames;
    p += BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
  }
  return frames;
}

bool Bpf::write(std::span<const uint8_t> frame) {
  return ::write(fd_, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size());
}

}  // namespace tl

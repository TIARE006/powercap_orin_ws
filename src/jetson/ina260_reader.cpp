#include "jetson/ina260_reader.hpp"
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace jetson {

Ina260Reader::Ina260Reader(std::string dev_path, int addr)
    : dev_path_(std::move(dev_path)), addr_(addr) {}

Ina260Reader::~Ina260Reader() {
    if (fd_ >= 0) ::close(fd_);
}

bool Ina260Reader::open_if_needed() {
    if (fd_ >= 0) return true;

    fd_ = ::open(dev_path_.c_str(), O_RDWR);
    if (fd_ < 0) return false;

    if (::ioctl(fd_, I2C_SLAVE, addr_) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

std::optional<uint16_t> Ina260Reader::read_reg16_be(uint8_t reg) {
    if (::write(fd_, &reg, 1) != 1) return std::nullopt;

    uint8_t buf[2] = {0, 0};
    if (::read(fd_, buf, 2) != 2) return std::nullopt;

    return static_cast<uint16_t>((buf[0] << 8) | buf[1]);
}

Ina260Sample Ina260Reader::read_sample() {
    Ina260Sample s;
    if (!open_if_needed()) return s;

    auto rc = read_reg16_be(0x01);
    auto rv = read_reg16_be(0x02);
    auto rp = read_reg16_be(0x03);
    if (!rc || !rv || !rp) return s;

    s.ok = true;
    s.current_a = (*rc * 1.25) / 1000.0;
    s.voltage_v = (*rv * 1.25) / 1000.0;
    s.power_w   = (*rp * 10.0) / 1000.0;
    return s;
}

} // namespace jetson
#pragma once
#include "jetson/types.hpp"
#include <optional>
#include <string>

namespace jetson {

class Ina260Reader {
public:
    Ina260Reader(std::string dev_path, int addr);
    ~Ina260Reader();

    bool open_if_needed();
    Ina260Sample read_sample();

private:
    std::optional<uint16_t> read_reg16_be(uint8_t reg);

    std::string dev_path_;
    int addr_;
    int fd_ = -1;
};

} // namespace jetson
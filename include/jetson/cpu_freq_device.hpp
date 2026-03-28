#pragma once
#include "jetson/types.hpp"
#include <optional>
#include <string>

namespace jetson {

class CpuFreqDevice {
public:
    explicit CpuFreqDevice(std::string policy_dir);

    static std::optional<CpuFreqDevice> discover();

    CpuState read_state() const;
    bool set_frequency(long khz) const;
    bool unlock() const;

    const std::string& path() const { return policy_dir_; }

private:
    std::string policy_dir_;
};

} // namespace jetson
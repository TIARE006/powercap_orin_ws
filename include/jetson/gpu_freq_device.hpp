#pragma once
#include "jetson/types.hpp"
#include <optional>
#include <string>

namespace jetson {

class GpuFreqDevice {
public:
    explicit GpuFreqDevice(std::string devfreq_dir);

    static std::optional<GpuFreqDevice> discover();

    GpuState read_state() const;
    bool set_frequency(long long hz) const;
    bool unlock() const;

    const std::string& path() const { return devfreq_dir_; }

private:
    static std::vector<long long> parse_available_frequencies(const std::string& s);

    std::string devfreq_dir_;
};

} // namespace jetson
#pragma once
#include "jetson/types.hpp"
#include <optional>
#include <string>

namespace jetson {

class FanDevice {
public:
    FanDevice(std::string cooling_dir, std::string pwm_path);

    static std::optional<FanDevice> discover();

    FanState read_state() const;

private:
    std::string cooling_dir_;
    std::string pwm_path_;
};

} // namespace jetson
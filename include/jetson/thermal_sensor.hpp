#pragma once
#include "jetson/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace jetson {

class ThermalSensor {
public:
    ThermalSensor(std::string name, std::string zone_dir);

    static std::vector<ThermalSensor> discover_all();

    ThermalReading read() const;

private:
    std::string name_;
    std::string zone_dir_;
};

} // namespace jetson
#pragma once
#include "jetson/cpu_freq_device.hpp"
#include "jetson/fan_device.hpp"
#include "jetson/gpu_freq_device.hpp"
#include "jetson/ina260_reader.hpp"
#include "jetson/tegrastats_monitor.hpp"
#include "jetson/thermal_sensor.hpp"
#include "jetson/types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace jetson {

class JetsonPlatform {
public:
    static std::optional<JetsonPlatform> discover();

    bool start_monitors(int tegrastats_interval_ms);
    void stop_monitors();

    PlatformSample sample();

    bool set_dvfs(const DvfsCommand& cmd);
    bool unlock_dvfs();

    const CpuFreqDevice& cpu() const { return cpu_; }
    const GpuFreqDevice& gpu() const { return gpu_; }

private:
    JetsonPlatform(CpuFreqDevice cpu,
                   GpuFreqDevice gpu,
                   std::optional<FanDevice> fan,
                   std::vector<ThermalSensor> thermals,
                   Ina260Reader ina260);

    CpuFreqDevice cpu_;
    GpuFreqDevice gpu_;
    std::optional<FanDevice> fan_;
    std::vector<ThermalSensor> thermals_;
    Ina260Reader ina260_;
    std::unique_ptr<TegrastatsMonitor> tegrastats_;
};

} // namespace jetson
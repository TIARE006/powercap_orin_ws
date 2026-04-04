#include "jetson/jetson_platform.hpp"
#include "common/time_utils.hpp"

#include <memory>
#include <utility>

namespace jetson {

JetsonPlatform::JetsonPlatform(CpuFreqDevice cpu,
                               GpuFreqDevice gpu,
                               std::optional<FanDevice> fan,
                               std::vector<ThermalSensor> thermals,
                               Ina260Reader ina260)
    : cpu_(std::move(cpu)),
      gpu_(std::move(gpu)),
      fan_(std::move(fan)),
      thermals_(std::move(thermals)),
      ina260_(std::move(ina260)),
      tegrastats_(std::make_unique<TegrastatsMonitor>()) {}

std::optional<JetsonPlatform> JetsonPlatform::discover() {
    auto cpu = CpuFreqDevice::discover();
    auto gpu = GpuFreqDevice::discover();
    if (!cpu || !gpu) return std::nullopt;

    auto fan = FanDevice::discover();
    auto thermals = ThermalSensor::discover_all();
    Ina260Reader ina("/dev/i2c-7", 0x40);

    return JetsonPlatform(std::move(*cpu),
                          std::move(*gpu),
                          std::move(fan),
                          std::move(thermals),
                          std::move(ina));
}

bool JetsonPlatform::start_monitors(int tegrastats_interval_ms) {
    return tegrastats_ && tegrastats_->start(tegrastats_interval_ms);
}

void JetsonPlatform::stop_monitors() {
    if (tegrastats_) {
        tegrastats_->stop();
    }
}

PlatformSample JetsonPlatform::sample() {
    PlatformSample s;
    s.ts_ns = common::now_ns();

    s.cpu = cpu_.read_state();
    s.gpu = gpu_.read_state();

    if (fan_) {
        s.fan = fan_->read_state();
    }

    for (const auto& t : thermals_) {
        s.thermal.zones.push_back(t.read());
    }

    if (tegrastats_) {
        s.power.tegrastats = tegrastats_->latest();
    }

    s.power.ina260 = ina260_.read_sample();
    return s;
}

// 新增：高频最小采样路径
PlatformSample JetsonPlatform::sample_minimal() {
    PlatformSample s;
    s.ts_ns = common::now_ns();

    if (tegrastats_) {
        s.power.tegrastats = tegrastats_->latest();
    }

    s.power.ina260 = ina260_.read_sample();
    return s;
}

bool JetsonPlatform::set_dvfs(const DvfsCommand& cmd) {
    bool ok = true;
    if (cmd.cpu_khz > 0) ok = ok && cpu_.set_frequency(cmd.cpu_khz);
    if (cmd.gpu_hz > 0) ok = ok && gpu_.set_frequency(cmd.gpu_hz);
    return ok;
}

bool JetsonPlatform::unlock_dvfs() {
    return cpu_.unlock() && gpu_.unlock();
}

} // namespace jetson
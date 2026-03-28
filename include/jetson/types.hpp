#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jetson {

struct CpuState {
    std::string governor;
    long cur_khz = -1;
    long min_khz = -1;
    long max_khz = -1;
    long hw_min_khz = -1;
    long hw_max_khz = -1;
};

struct GpuState {
    std::string governor;
    long long cur_hz = -1;
    long long min_hz = -1;
    long long max_hz = -1;
    std::vector<long long> available_hz;
};

struct FanState {
    long cur_state = -1;
    long max_state = -1;
    long pwm = -1;
};

struct ThermalReading {
    std::string name;
    long temp_mc = -1;
};

struct ThermalState {
    std::vector<ThermalReading> zones;
};

struct TegrastatsPower {
    long long vdd_in_mw = -1;
    long long vdd_cpu_gpu_cv_mw = -1;
    long long vdd_soc_mw = -1;
};

struct Ina260Sample {
    bool ok = false;
    double current_a = 0.0;
    double voltage_v = 0.0;
    double power_w = 0.0;
};

struct PowerState {
    TegrastatsPower tegrastats;
    Ina260Sample ina260;
};

struct PlatformSample {
    int64_t ts_ns = 0;
    CpuState cpu;
    GpuState gpu;
    FanState fan;
    ThermalState thermal;
    PowerState power;
};

struct DvfsCommand {
    long cpu_khz = -1;
    long long gpu_hz = -1;
};

} // namespace jetson
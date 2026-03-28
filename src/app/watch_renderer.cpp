#include "app/watch_renderer.hpp"
#include <cstdio>
#include <iostream>
#include <iomanip>

namespace app {

static std::string fmt_temp(long mc) {
    if (mc < 0) return "NA";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", mc / 1000.0);
    return std::string(buf);
}

static std::string fmt_fp(double x) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", x);
    return std::string(buf);
}

void WatchRenderer::render(const jetson::PlatformSample& s, const jetson::JtopSample& jt) {
    if (!initialized_) {
        std::cerr << "\n\n\n\n\n\n\n";
        initialized_ = true;
    } else {
        std::cerr << "\033[7A";
    }

    std::cerr << "\033[2K\r"
              << "CPU: cur=" << s.cpu.cur_khz
              << " min=" << s.cpu.min_khz
              << " max=" << s.cpu.max_khz
              << " gov=" << s.cpu.governor << "\n";

    std::cerr << "\033[2K\r"
              << "GPU: cur=" << s.gpu.cur_hz
              << " min=" << s.gpu.min_hz
              << " max=" << s.gpu.max_hz
              << " gov=" << s.gpu.governor << "\n";

    std::cerr << "\033[2K\r"
              << "FAN: state=" << s.fan.cur_state << "/" << s.fan.max_state
              << " pwm=" << s.fan.pwm << "\n";

    long cpu_t = -1, gpu_t = -1;
    for (const auto& z : s.thermal.zones) {
        if (z.name.find("cpu") != std::string::npos) cpu_t = z.temp_mc;
        if (z.name.find("gpu") != std::string::npos) gpu_t = z.temp_mc;
    }

    std::cerr << "\033[2K\r"
              << "TEMP: CPU=" << fmt_temp(cpu_t)
              << "C GPU=" << fmt_temp(gpu_t) << "C\n";

    std::cerr << "\033[2K\r"
              << "POWER: VDD_IN=" << s.power.tegrastats.vdd_in_mw
              << "mW VDD_CPU_GPU_CV=" << s.power.tegrastats.vdd_cpu_gpu_cv_mw
              << "mW VDD_SOC=" << s.power.tegrastats.vdd_soc_mw << "mW\n";

    std::cerr << "\033[2K\r"
              << "JTOP_POWER: TOT="
              << (jt.valid ? jt.power_tot_mw : "NA")
              << "mW CPU_GPU_CV="
              << (jt.valid ? jt.power_vdd_cpu_gpu_cv_mw : "NA")
              << "mW SOC="
              << (jt.valid ? jt.power_vdd_soc_mw : "NA")
              << "mW\n";

    std::cerr << "\033[2K\r"
              << "INA260: I="
              << (s.power.ina260.ok ? fmt_fp(s.power.ina260.current_a) : "NA")
              << "A V="
              << (s.power.ina260.ok ? fmt_fp(s.power.ina260.voltage_v) : "NA")
              << "V P="
              << (s.power.ina260.ok ? fmt_fp(s.power.ina260.power_w) : "NA")
              << "W\n";

    std::cerr << std::flush;
}

} // namespace app
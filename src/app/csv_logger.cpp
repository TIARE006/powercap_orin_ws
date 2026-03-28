#include "app/csv_logger.hpp"
#include <iomanip>

namespace app {

CsvLogger::CsvLogger(const std::string& path)
    : ofs_(path) {}

bool CsvLogger::ok() const {
    return static_cast<bool>(ofs_);
}

void CsvLogger::write_header() {
    ofs_ << "ts_ns,"
         << "cpu_khz,cpu_min_khz,cpu_max_khz,cpu_governor,"
         << "gpu_hz,gpu_min_hz,gpu_max_hz,gpu_governor,"
         << "fan_cur_state,fan_max_state,fan_pwm,"
         << "temp_cpu_mC,temp_gpu_mC,temp_soc0_mC,temp_soc1_mC,temp_soc2_mC,temp_tj_mC,"
         << "vdd_in_mW,vdd_cpu_gpu_cv_mW,vdd_soc_mW,"
         << "ina260_current_A,ina260_voltage_V,ina260_power_W,"
         << "jtop_valid,jtop_host_ts_ns,"
         << "jtop_power_tot_mw,jtop_power_vdd_cpu_gpu_cv_mw,jtop_power_vdd_soc_mw\n";
}

long CsvLogger::find_temp(const jetson::ThermalState& t, const std::string& key) {
    for (const auto& z : t.zones) {
        if (z.name.find(key) != std::string::npos) return z.temp_mc;
    }
    return -1;
}

void CsvLogger::write_sample(const jetson::PlatformSample& s) {
    jetson::JtopSample jt;
    write_sample(s, jt);
}

void CsvLogger::write_sample(const jetson::PlatformSample& s, const jetson::JtopSample& jt) {
    ofs_ << s.ts_ns << ","
         << s.cpu.cur_khz << "," << s.cpu.min_khz << "," << s.cpu.max_khz << "," << s.cpu.governor << ","
         << s.gpu.cur_hz << "," << s.gpu.min_hz << "," << s.gpu.max_hz << "," << s.gpu.governor << ","
         << s.fan.cur_state << "," << s.fan.max_state << "," << s.fan.pwm << ","
         << find_temp(s.thermal, "cpu") << ","
         << find_temp(s.thermal, "gpu") << ","
         << find_temp(s.thermal, "soc0") << ","
         << find_temp(s.thermal, "soc1") << ","
         << find_temp(s.thermal, "soc2") << ","
         << find_temp(s.thermal, "tj") << ","
         << s.power.tegrastats.vdd_in_mw << ","
         << s.power.tegrastats.vdd_cpu_gpu_cv_mw << ","
         << s.power.tegrastats.vdd_soc_mw << ","
         << (s.power.ina260.ok ? s.power.ina260.current_a : -1.0) << ","
         << (s.power.ina260.ok ? s.power.ina260.voltage_v : -1.0) << ","
         << (s.power.ina260.ok ? s.power.ina260.power_w : -1.0) << ","
         << (jt.valid ? 1 : 0) << ","
         << jt.host_ts_ns << ","
         << (jt.valid ? jt.power_tot_mw : "") << ","
         << (jt.valid ? jt.power_vdd_cpu_gpu_cv_mw : "") << ","
         << (jt.valid ? jt.power_vdd_soc_mw : "")
         << "\n";
}

} // namespace app
#include "jetson/tegrastats_monitor.hpp"
#include <cstdio>
#include <string>
#include <unistd.h>

namespace jetson {

TegrastatsMonitor::~TegrastatsMonitor() {
    stop();
}

std::optional<long long> TegrastatsMonitor::parse_mw_field(const std::string& line, const std::string& key) {
    auto pos = line.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos += key.size();

    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;

    long long val = 0;
    bool any = false;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        any = true;
        val = val * 10 + (line[pos] - '0');
        pos++;
    }
    if (!any) return std::nullopt;
    if (pos + 1 >= line.size() || line[pos] != 'm' || line[pos + 1] != 'W') return std::nullopt;

    return val;
}

bool TegrastatsMonitor::start(int interval_ms) {
    if (running_) return true;
    running_ = true;
    worker_ = std::thread(&TegrastatsMonitor::run, this, interval_ms);
    return true;
}

void TegrastatsMonitor::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

TegrastatsPower TegrastatsMonitor::latest() const {
    TegrastatsPower p;
    p.vdd_in_mw = vdd_in_mw_.load(std::memory_order_relaxed);
    p.vdd_cpu_gpu_cv_mw = vdd_cpu_gpu_cv_mw_.load(std::memory_order_relaxed);
    p.vdd_soc_mw = vdd_soc_mw_.load(std::memory_order_relaxed);
    return p;
}

void TegrastatsMonitor::run(int interval_ms) {
    std::string cmd;
    if (::geteuid() == 0) cmd = "tegrastats --interval " + std::to_string(interval_ms);
    else                  cmd = "sudo tegrastats --interval " + std::to_string(interval_ms);

    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        running_ = false;
        return;
    }

    char buf[8192];
    while (running_ && std::fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        if (auto v = parse_mw_field(line, "VDD_IN")) vdd_in_mw_.store(*v, std::memory_order_relaxed);
        if (auto v = parse_mw_field(line, "VDD_CPU_GPU_CV")) vdd_cpu_gpu_cv_mw_.store(*v, std::memory_order_relaxed);
        if (auto v = parse_mw_field(line, "VDD_SOC")) vdd_soc_mw_.store(*v, std::memory_order_relaxed);
    }

    ::pclose(fp);
    running_ = false;
}

} // namespace jetson
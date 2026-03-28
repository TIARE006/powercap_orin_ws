#pragma once
#include "jetson/types.hpp"
#include <atomic>
#include <thread>

namespace jetson {

class TegrastatsMonitor {
public:
    TegrastatsMonitor() = default;
    ~TegrastatsMonitor();

    bool start(int interval_ms);
    void stop();

    TegrastatsPower latest() const;

private:
    static std::optional<long long> parse_mw_field(const std::string& line, const std::string& key);
    void run(int interval_ms);

    std::atomic<bool> running_{false};
    std::thread worker_;

    std::atomic<long long> vdd_in_mw_{-1};
    std::atomic<long long> vdd_cpu_gpu_cv_mw_{-1};
    std::atomic<long long> vdd_soc_mw_{-1};
};

} // namespace jetson
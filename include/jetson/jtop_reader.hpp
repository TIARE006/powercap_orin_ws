#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace jetson {

struct JtopSample {
    bool valid = false;
    uint64_t host_ts_ns = 0;

    std::string power_tot_mw;
    std::string power_vdd_cpu_gpu_cv_mw;
    std::string power_vdd_soc_mw;
};

class JtopReader {
public:
    JtopReader() = default;
    ~JtopReader();

    bool start(const std::string& cmd);
    void stop();
    JtopSample latest() const;

private:
    void reader_loop();

    FILE* pipe_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    JtopSample latest_;
};

} // namespace jetson
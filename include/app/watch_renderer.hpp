#pragma once
#include "jetson/types.hpp"
#include "jetson/jtop_reader.hpp"

namespace app {

class WatchRenderer {
public:
    void render(const jetson::PlatformSample& s, const jetson::JtopSample& jt);

private:
    bool initialized_ = false;
};

} // namespace app
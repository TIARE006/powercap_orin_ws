#pragma once
#include "jetson/types.hpp"

namespace app {

class WatchRenderer {
public:
    void render(const jetson::PlatformSample& s);

private:
    bool initialized_ = false;
};

} // namespace app
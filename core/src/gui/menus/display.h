#pragma once
#include <module.h>

namespace displaymenu {
    void init();
    void checkKeybinds();
    void draw(void* ctx);

    // MPX 分析默认参数（唯一来源）
    const int MPX_DEFAULT_REFRESH_RATE = 10;
    const float MPX_DEFAULT_LINE_WIDTH = 2.5f;
    const int MPX_DEFAULT_SMOOTHING_FACTOR = 3;

    // MPX 分析全局设置（跨平台导出/导入）
    SDRPP_EXPORT int mpxRefreshRate;
    SDRPP_EXPORT float mpxLineWidth;
    SDRPP_EXPORT int mpxSmoothingFactor;
}
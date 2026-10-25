#pragma once

// 左侧工作区的触摸滑动支持（仅 Android 使用）。
//
// 触摸在 ImGui 里被当成鼠标：手指一按下，控件（按钮、滑块等）就立刻被激活，
// 所以光靠 ImGui 没法区分“点按/拖滑块”和“上下滑动列表”。
// 做法是在输入层把落在左侧面板内的触摸先扣住，等手指移动超过阈值再判断方向：
//   - 竖直方向为主：当作滚动手势，不交给 ImGui，只累计滚动量，由主窗口应用到面板；
//   - 水平方向为主：把按下事件补发到最初的按下位置，之后按普通鼠标拖动处理（比如拖滑块）；
//   - 没移动就抬起：当作一次点按，补发按下 + 抬起。
// 这里只保存状态；手势判断在 core/backends/android/backend.cpp 里，
// 面板位置和滚动量的读写在 core/src/gui/main_window.cpp 里。

namespace touchscroll {
    struct State {
        // 面板在屏幕上的矩形（主窗口每帧更新）。valid=false 表示面板不可见或被弹窗挡住，手势不生效。
        bool valid = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float slop = 24.0f; // 手指移动超过多少像素才开始判断方向

        // 手势状态（仅输入层使用）
        enum Phase { IDLE, PENDING, SCROLLING, PASSTHROUGH };
        Phase phase = IDLE;
        float pressX = 0, pressY = 0;
        float lastY = 0;

        // 尚未应用的滚动量（像素，正值表示内容向上滚动，即显示更靠下的内容）
        float pendingScroll = 0;
    };

    inline State& state() {
        static State s;
        return s;
    }

    // 主窗口每帧调用：设置面板矩形
    inline void setPanel(float x0, float y0, float x1, float y1, float slop) {
        State& s = state();
        s.valid = (x1 > x0 && y1 > y0);
        s.x0 = x0;
        s.y0 = y0;
        s.x1 = x1;
        s.y1 = y1;
        s.slop = slop;
    }

    // 面板不可见或有弹窗时调用
    inline void clearPanel() {
        state().valid = false;
    }

    // 主窗口每帧调用：取走累计的滚动量
    inline float consumeScroll() {
        State& s = state();
        float v = s.pendingScroll;
        s.pendingScroll = 0;
        return v;
    }

    inline bool isScrolling() {
        return state().phase == State::SCROLLING;
    }
}

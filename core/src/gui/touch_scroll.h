#pragma once

// 左侧工作区的触摸滑动支持（仅 Android 使用）。
//
// 触摸在 ImGui 里被当成鼠标：手指一按下，控件（按钮、滑块等）就立刻被激活，
// 所以光靠 ImGui 没法区分“点按/拖滑块”和“上下滑动列表”。
// 做法是在输入层把落在左侧面板内的触摸先扣住，等手指移动超过阈值再判断方向：
//   - 竖直方向为主：当作滚动手势，不交给 ImGui，只累计滚动量，由主窗口应用到面板；
//   - 水平方向为主：把按下事件补发到最初的按下位置，之后按普通鼠标拖动处理（比如拖滑块）；
//   - 没移动就抬起：当作一次点按，补发按下 + 抬起。
// 另外两种特殊区域：
//   - 内部滚动区（inner）：面板里带自己滚动条的列表（例如频率管理器的书签表格）。
//     手指在列表上竖直滑动时，滚动的是这个列表而不是整个面板；如果列表已经滑到头、
//     手指还往外拉，则交给整个面板滚动（和手机上常见的嵌套滚动一致）。
//   - 拖动手柄（drag handle）：菜单标题栏。菜单顺序未锁定时，按在标题栏上的触摸直接交给 ImGui，
//     这样才能上下拖动标题来调整菜单顺序（否则竖直拖动会被当成面板滚动）。
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

        // 面板内各模块自己的滚动条（例如模块管理器、频率管理器里的列表）所在的矩形。
        // 手指按在这些区域时不做手势判断，直接交给 ImGui，这样滚动条才能被拖动。主窗口每帧更新。
        static const int kMaxExclusions = 16;
        int exclusionCount = 0;
        float exclusions[kMaxExclusions][4] = {};

        // 面板内带滚动条的子窗口（列表）的可见区域。手指竖直滑动落在这里时优先滚动该列表。
        // id 是 ImGui 窗口 ID；scrollY / scrollMax 是该列表当前的滚动位置和最大滚动量（主窗口每帧更新）。
        static const int kMaxInner = 8;
        int innerCount = 0;
        float innerRects[kMaxInner][4] = {};
        unsigned int innerIds[kMaxInner] = {};
        float innerScrollY[kMaxInner] = {};
        float innerScrollMax[kMaxInner] = {};

        // 当前手势滚动的目标列表（0 表示滚动整个面板）以及尚未应用的列表滚动量
        unsigned int target = 0;
        float pendingInnerScroll = 0;

        // 菜单标题栏的矩形（仅在菜单顺序未锁定时由菜单每帧登记）
        static const int kMaxDragHandles = 64;
        int dragHandleCount = 0;
        float dragHandles[kMaxDragHandles][4] = {};
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

    // 主窗口每帧调用：清空 / 添加“不参与手势”的矩形（子窗口滚动条）
    inline void clearExclusions() {
        state().exclusionCount = 0;
    }

    inline void addExclusion(float x0, float y0, float x1, float y1) {
        State& s = state();
        if (s.exclusionCount >= State::kMaxExclusions) { return; }
        float* r = s.exclusions[s.exclusionCount++];
        r[0] = x0;
        r[1] = y0;
        r[2] = x1;
        r[3] = y1;
    }

    inline bool inExclusion(float x, float y) {
        const State& s = state();
        for (int i = 0; i < s.exclusionCount; i++) {
            const float* r = s.exclusions[i];
            if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) { return true; }
        }
        return false;
    }

    // 主窗口每帧调用：清空 / 添加“内部滚动区”（带滚动条的列表）
    inline void clearInner() {
        state().innerCount = 0;
    }

    inline void addInner(unsigned int id, float x0, float y0, float x1, float y1, float scrollY, float scrollMax) {
        State& s = state();
        if (s.innerCount >= State::kMaxInner) { return; }
        int i = s.innerCount++;
        s.innerRects[i][0] = x0;
        s.innerRects[i][1] = y0;
        s.innerRects[i][2] = x1;
        s.innerRects[i][3] = y1;
        s.innerIds[i] = id;
        s.innerScrollY[i] = scrollY;
        s.innerScrollMax[i] = scrollMax;
    }

    // 返回包含该点的内部滚动区下标（后登记的是更内层的，优先），没有返回 -1
    inline int findInner(float x, float y) {
        const State& s = state();
        for (int i = s.innerCount - 1; i >= 0; i--) {
            const float* r = s.innerRects[i];
            if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) { return i; }
        }
        return -1;
    }

    // 主窗口每帧调用：取走累计的列表滚动量，并返回它对应的 ImGui 窗口 ID
    inline float consumeInnerScroll(unsigned int* id) {
        State& s = state();
        float v = s.pendingInnerScroll;
        s.pendingInnerScroll = 0;
        if (id) { *id = s.target; }
        return v;
    }

    // 菜单每帧开始绘制前清空，绘制每个标题栏时登记
    inline void clearDragHandles() {
        state().dragHandleCount = 0;
    }

    inline void addDragHandle(float x0, float y0, float x1, float y1) {
        State& s = state();
        if (s.dragHandleCount >= State::kMaxDragHandles) { return; }
        float* r = s.dragHandles[s.dragHandleCount++];
        r[0] = x0;
        r[1] = y0;
        r[2] = x1;
        r[3] = y1;
    }

    inline bool inDragHandle(float x, float y) {
        const State& s = state();
        for (int i = 0; i < s.dragHandleCount; i++) {
            const float* r = s.dragHandles[i];
            if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) { return true; }
        }
        return false;
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

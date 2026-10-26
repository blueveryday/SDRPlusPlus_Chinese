#include <backend.h>log
#include "android_backend.h"
#include <core.h>
#include <gui/gui.h>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <gui/icons.h>
#include <gui/style.h>
#include <gui/menus/theme.h>
#include <gui/touch_scroll.h>
#include <imgui_internal.h>
#include <utils/i18n.h>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

// Credit to the ImGui android OpenGL3 example for a lot of this code!

namespace backend {
    struct android_app* app = NULL;
    EGLDisplay _EglDisplay = EGL_NO_DISPLAY;
    EGLSurface _EglSurface = EGL_NO_SURFACE;
    EGLContext _EglContext = EGL_NO_CONTEXT;
    bool _Initialized = false;
    char _LogTag[] = "SDR++";
    bool initialized = false;
    bool pauseRendering = false;
    bool exited = false;

    // Forward declaration
    int ShowSoftKeyboardInput();
    int PollUnicodeChars();
    int HideSoftKeyboardInput();
    void ProcessPendingDialogResults();
    // 输入框编辑工具栏与系统剪贴板（见下面 backend 命名空间里的实现）
    const char* androidGetClipboard(void* userData);
    void androidSetClipboard(void* userData, const char* text);
    void updateEditBarLabels();
    void processEditActions();

    void doPartialInit() {
        std::string root = (std::string)core::args["root"];
        backend::init();
        style::loadFonts(root + "/res"); // TODO: Don't hardcode, use config
        icons::load(root + "/res");
        thememenu::applyTheme();
        ImGui::GetStyle().ScaleAllSizes(style::uiScale);
        gui::mainWindow.setFirstMenuRender();
    }

    void handleAppCmd(struct android_app* app, int32_t appCmd) {
        switch (appCmd) {
        case APP_CMD_SAVE_STATE:
            flog::warn("APP_CMD_SAVE_STATE");
            break;
        case APP_CMD_INIT_WINDOW:
            flog::warn("APP_CMD_INIT_WINDOW");
            if (pauseRendering && !exited) {
                doPartialInit();
                pauseRendering = false;
            }
            exited = false;
            break;
        case APP_CMD_TERM_WINDOW:
            flog::warn("APP_CMD_TERM_WINDOW");
            pauseRendering = true;
            backend::end();
            break;
        case APP_CMD_GAINED_FOCUS:
            flog::warn("APP_CMD_GAINED_FOCUS");
            break;
        case APP_CMD_LOST_FOCUS:
            flog::warn("APP_CMD_LOST_FOCUS");
            break;
        }
    }

    // 本次手势按下时所在的内部滚动区下标（-1 表示不在任何列表上）
    static int s_pressInner = -1;

    // 左侧面板的触摸滑动手势（详见 gui/touch_scroll.h）。
    // 返回 true 表示事件已处理，不再交给 ImGui。
    static bool handleTouchScroll(AInputEvent* ev) {
        if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) { return false; }

        touchscroll::State& st = touchscroll::state();
        ImGuiIO& io = ImGui::GetIO();

        int32_t action = AMotionEvent_getAction(ev);
        int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
        float x = AMotionEvent_getX(ev, 0);
        float y = AMotionEvent_getY(ev, 0);

        // 只处理手指/手写笔/未知类型；实体鼠标保持原有行为（滚轮本来就能滚动）
        int32_t tool = AMotionEvent_getToolType(ev, 0);
        bool isTouch = (tool == AMOTION_EVENT_TOOL_TYPE_FINGER) || (tool == AMOTION_EVENT_TOOL_TYPE_STYLUS) || (tool == AMOTION_EVENT_TOOL_TYPE_UNKNOWN);

        switch (actionMasked) {
        case AMOTION_EVENT_ACTION_DOWN: {
            // 上一次手势异常没收尾时，先收尾
            if (st.phase == touchscroll::State::PASSTHROUGH) { io.AddMouseButtonEvent(0, false); }
            st.phase = touchscroll::State::IDLE;

            if (!isTouch || !st.valid) { return false; }
            if (x < st.x0 || x >= st.x1 || y < st.y0 || y >= st.y1) { return false; }
            // 按在模块自己的滚动条上：直接交给 ImGui，让滚动条可以拖动
            if (touchscroll::inExclusion(x, y)) { return false; }
            // 菜单顺序未锁定时，按在菜单标题栏上：直接交给 ImGui，这样才能上下拖动标题调整菜单顺序
            if (touchscroll::inDragHandle(x, y)) { return false; }

            // 记录按下的位置在哪个内部滚动区（频率管理器的列表等），竖直滑动时优先滚动它
            s_pressInner = touchscroll::findInner(x, y);
            st.target = 0;

            st.phase = touchscroll::State::PENDING;
            st.pressX = x;
            st.pressY = y;
            st.lastY = y;
            // 只更新指针位置（让悬停高亮跟手），不发按下
            io.AddMousePosEvent(x, y);
            return true;
        }
        case AMOTION_EVENT_ACTION_MOVE: {
            if (st.phase == touchscroll::State::PENDING) {
                float dx = x - st.pressX;
                float dy = y - st.pressY;
                if ((dx * dx) + (dy * dy) < (st.slop * st.slop)) { return true; }

                if (fabsf(dy) > fabsf(dx)) {
                    // 竖直方向为主：开始滚动。把指针移出屏幕，避免控件保持悬停高亮
                    st.phase = touchscroll::State::SCROLLING;
                    st.lastY = y;
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

                    // 决定滚动谁：按在列表上且列表还能往手指拖动的方向滚，就滚列表；
                    // 列表已经在头/尾、手指还往外拉时，滚动整个面板
                    st.target = 0;
                    if (s_pressInner >= 0) {
                        float sy = st.innerScrollY[s_pressInner];
                        float sm = st.innerScrollMax[s_pressInner];
                        bool canScroll = (dy > 0.0f) ? (sy > 0.5f) : (sy < sm - 0.5f);
                        if (canScroll) { st.target = st.innerIds[s_pressInner]; }
                    }
                }
                else {
                    // 水平方向为主：补发按下（位置为最初按下的点），后续按普通拖动处理
                    st.phase = touchscroll::State::PASSTHROUGH;
                    io.AddMousePosEvent(st.pressX, st.pressY);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMousePosEvent(x, y);
                }
                return true;
            }
            if (st.phase == touchscroll::State::SCROLLING) {
                // 手指向上移动，内容跟着向上，即滚动位置增大
                if (st.target != 0) { st.pendingInnerScroll += st.lastY - y; }
                else { st.pendingScroll += st.lastY - y; }
                st.lastY = y;
                return true;
            }
            return false; // PASSTHROUGH / IDLE：交给 ImGui 处理
        }
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: {
            bool cancel = (actionMasked == AMOTION_EVENT_ACTION_CANCEL);
            touchscroll::State::Phase phase = st.phase;
            st.phase = touchscroll::State::IDLE;

            if (phase == touchscroll::State::PENDING) {
                // 没怎么移动：当作一次点按，补发按下 + 抬起（ImGui 会分到相邻两帧处理）
                if (!cancel) {
                    io.AddMousePosEvent(st.pressX, st.pressY);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                }
                return true;
            }
            if (phase == touchscroll::State::SCROLLING) {
                return true;
            }
            if (phase == touchscroll::State::PASSTHROUGH) {
                io.AddMousePosEvent(x, y);
                io.AddMouseButtonEvent(0, false);
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    int32_t handleInputEvent(struct android_app* app, AInputEvent* inputEvent) {
        if (handleTouchScroll(inputEvent)) { return 1; }
        return ImGui_ImplAndroid_HandleInputEvent(inputEvent);
    }

    int aquireWindow() {
        while (!app->window) {
            flog::warn("Waiting on the shitty window thing"); std::this_thread::sleep_for(std::chrono::milliseconds(30));
            int out_events;
            struct android_poll_source* out_data;

            while (ALooper_pollAll(0, NULL, &out_events, (void**)&out_data) >= 0) {
                // Process one event
                if (out_data != NULL) { out_data->process(app, out_data); }

                // Exit the app by returning from within the infinite loop
                if (app->destroyRequested != 0) {
                    return -1;
                }
            }
        }
        ANativeWindow_acquire(app->window);
        return 0;
    }

    int init(std::string resDir) {
        flog::warn("Backend init");

        // Get window
        aquireWindow();

        // EGL Init
        {
            _EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (_EglDisplay == EGL_NO_DISPLAY)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");

            if (eglInitialize(_EglDisplay, 0, 0) != EGL_TRUE)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglInitialize() returned with an error");

            const EGLint egl_attributes[] = { EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
            EGLint num_configs = 0;
            if (eglChooseConfig(_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglChooseConfig() returned with an error");
            if (num_configs == 0)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglChooseConfig() returned 0 matching config");

            // Get the first matching config
            EGLConfig egl_config;
            eglChooseConfig(_EglDisplay, egl_attributes, &egl_config, 1, &num_configs);
            EGLint egl_format;
            eglGetConfigAttrib(_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &egl_format);
            ANativeWindow_setBuffersGeometry(app->window, 0, 0, egl_format);

            const EGLint egl_context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            _EglContext = eglCreateContext(_EglDisplay, egl_config, EGL_NO_CONTEXT, egl_context_attributes);

            if (_EglContext == EGL_NO_CONTEXT)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglCreateContext() returned EGL_NO_CONTEXT");

            _EglSurface = eglCreateWindowSurface(_EglDisplay, egl_config, app->window, NULL);
            eglMakeCurrent(_EglDisplay, _EglSurface, _EglSurface, _EglContext);
        }

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;

        // Disable loading/saving of .ini file from disk.
        // FIXME: Consider using LoadIniSettingsFromMemory() / SaveIniSettingsToMemory() to save in appropriate location for Android.
        io.IniFilename = NULL;

        // 输入框的复制/剪切/粘贴使用系统剪贴板
        io.GetClipboardTextFn = androidGetClipboard;
        io.SetClipboardTextFn = androidSetClipboard;

        // Setup Platform/Renderer backends
        ImGui_ImplAndroid_Init(app->window);
        ImGui_ImplOpenGL3_Init("#version 300 es");

        return 0;
    }

    void beginFrame() {
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
    }

    void render(bool vsync) {
        // Rendering
        ImGui::Render();
        auto dSize = ImGui::GetIO().DisplaySize;
        glViewport(0, 0, dSize.x, dSize.y);
        glClearColor(gui::themeManager.clearColor.x, gui::themeManager.clearColor.y, gui::themeManager.clearColor.z, gui::themeManager.clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(_EglDisplay, _EglSurface);
    }

    // No screen pos to detect
    void getMouseScreenPos(double& x, double& y) { x = 0; y = 0; }
    void setMouseScreenPos(double x, double y) {}

    int renderLoop() {
        while (true) {
            int out_events;
            struct android_poll_source* out_data;

            while (ALooper_pollAll(0, NULL, &out_events, (void**)&out_data) >= 0) {
                // Process one event
                if (out_data != NULL) { out_data->process(app, out_data); }

                // Exit the app by returning from within the infinite loop
                if (app->destroyRequested != 0) {
                    flog::warn("ASKED TO EXIT");
                    exited = true;

                    // Stop SDR
                    gui::mainWindow.setPlayState(false);
                    return 0;
                }
            }

            if (_EglDisplay == EGL_NO_DISPLAY) {
                // 窗口已销毁（屏幕熄灭或切到后台）时不要空转，否则会占满一个 CPU 核心
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            if (!pauseRendering) {
                // Initiate a new frame
                ImGuiIO& io = ImGui::GetIO();
                auto dsize = io.DisplaySize;

                // Poll Unicode characters via JNI
                // FIXME: do not call this every frame because of JNI overhead
                PollUnicodeChars();

                // 在渲染线程处理文件选择器返回的结果（导入/导出）
                ProcessPendingDialogResults();

                // Open on-screen (soft) input if requested by Dear ImGui
                static bool WantTextInputLast = false;
                if (io.WantTextInput && !WantTextInputLast) {
                    updateEditBarLabels();
                    ShowSoftKeyboardInput();
                }
                else if (!io.WantTextInput && WantTextInputLast) {
                    HideSoftKeyboardInput();
                }
                WantTextInputLast = io.WantTextInput;

                // 处理输入框编辑工具栏（全选/复制/剪切/粘贴/删除）点出来的操作
                processEditActions();

                // Render
                beginFrame();
                
                if (dsize.x > 0 && dsize.y > 0) {
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(dsize.x, dsize.y));
                    gui::mainWindow.draw();
                }
                render();
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }

        return 0;
    }

    int end() {
        // Cleanup
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();

        // Destroy all
        if (_EglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (_EglContext != EGL_NO_CONTEXT) { eglDestroyContext(_EglDisplay, _EglContext); }
            if (_EglSurface != EGL_NO_SURFACE) { eglDestroySurface(_EglDisplay, _EglSurface); }
            eglTerminate(_EglDisplay);
        }

        _EglDisplay = EGL_NO_DISPLAY;
        _EglContext = EGL_NO_CONTEXT;
        _EglSurface = EGL_NO_SURFACE;

        if (app->window) { ANativeWindow_release(app->window); }

        return 0;
    }

    int ShowSoftKeyboardInput() {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;

        jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (jni_return == JNI_ERR)
            return -1;

        jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
        if (jni_return != JNI_OK)
            return -2;

        jclass native_activity_clazz = java_env->GetObjectClass(app->activity->clazz);
        if (native_activity_clazz == NULL)
            return -3;

        jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "showSoftInput", "()V");
        if (method_id == NULL)
            return -4;

        java_env->CallVoidMethod(app->activity->clazz, method_id);

        jni_return = java_vm->DetachCurrentThread();
        if (jni_return != JNI_OK)
            return -5;

        return 0;
    }

    // 通知 Java 端收起软键盘（输入框失去焦点时调用）
    int HideSoftKeyboardInput() {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;

        jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (jni_return == JNI_ERR)
            return -1;

        jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
        if (jni_return != JNI_OK)
            return -2;

        jclass native_activity_clazz = java_env->GetObjectClass(app->activity->clazz);
        if (native_activity_clazz == NULL)
            return -3;

        jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "hideSoftInput", "()V");
        if (method_id == NULL)
            return -4;

        java_env->CallVoidMethod(app->activity->clazz, method_id);

        jni_return = java_vm->DetachCurrentThread();
        if (jni_return != JNI_OK)
            return -5;

        return 0;
    }

    int getDeviceFD(int& vid, int& pid, const std::vector<DevVIDPID>& allowedVidPids) {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;

        jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (jni_return == JNI_ERR)
            return -1;

        jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
        if (jni_return != JNI_OK)
            return -1;

        jclass native_activity_clazz = java_env->GetObjectClass(app->activity->clazz);
        if (native_activity_clazz == NULL)
            return -1;

        jfieldID fd_field_id = java_env->GetFieldID(native_activity_clazz, "SDR_FD", "I");
        jfieldID vid_field_id = java_env->GetFieldID(native_activity_clazz, "SDR_VID", "I");
        jfieldID pid_field_id = java_env->GetFieldID(native_activity_clazz, "SDR_PID", "I");
        
        if (!vid_field_id || !vid_field_id || !pid_field_id)
            return -1;

        int fd = java_env->GetIntField(app->activity->clazz, fd_field_id);
        vid = java_env->GetIntField(app->activity->clazz, vid_field_id);
        pid = java_env->GetIntField(app->activity->clazz, pid_field_id);

        jni_return = java_vm->DetachCurrentThread();
        if (jni_return != JNI_OK)
            return -1;

        // If no vid/pid was given, just return successfully
        if (allowedVidPids.empty()) {
            return fd;
        }

        // Otherwise, check that the vid/pid combo is allowed
        for (auto const& vp : allowedVidPids) {
            if (vp.vid != vid || vp.pid != pid) { continue; }
            return fd;
        }

        return -1;
    }

    // Unfortunately, the native KeyEvent implementation has no getUnicodeChar() function.
    // Therefore, we implement the processing of KeyEvents in MainActivity.kt and poll
    // the resulting Unicode characters here via JNI and send them to Dear ImGui.
    int PollUnicodeChars() {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;

        jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (jni_return == JNI_ERR)
            return -1;

        jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
        if (jni_return != JNI_OK)
            return -2;

        jclass native_activity_clazz = java_env->GetObjectClass(app->activity->clazz);
        if (native_activity_clazz == NULL)
            return -3;

        jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "pollUnicodeChar", "()I");
        if (method_id == NULL)
            return -4;

        // Send the actual characters to Dear ImGui
        ImGuiIO& io = ImGui::GetIO();
        jint unicode_character;
        while ((unicode_character = java_env->CallIntMethod(app->activity->clazz, method_id)) != 0) {
            // 退格(8)和回车(10)不是可输入字符，ImGui 会丢弃它们，需要转换成按键事件
            if (unicode_character == 8) {
                io.AddKeyEvent(ImGuiKey_Backspace, true);
                io.AddKeyEvent(ImGuiKey_Backspace, false);
            }
            else if (unicode_character == 10) {
                io.AddKeyEvent(ImGuiKey_Enter, true);
                io.AddKeyEvent(ImGuiKey_Enter, false);
            }
            else {
                io.AddInputCharacter(unicode_character);
            }
        }

        jni_return = java_vm->DetachCurrentThread();
        if (jni_return != JNI_OK)
            return -5;

        return 0;
    }

    std::string getAppFilesDir() {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;

        jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (jni_return == JNI_ERR)
            throw std::runtime_error("Could not get JNI environment");

        jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
        if (jni_return != JNI_OK)
            throw std::runtime_error("Could not attach to thread");

        jclass native_activity_clazz = java_env->GetObjectClass(app->activity->clazz);
        if (native_activity_clazz == NULL)
            throw std::runtime_error("Could not get MainActivity class");

        jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "getAppDir", "()Ljava/lang/String;");
        if (method_id == NULL)
            throw std::runtime_error("Could not get method ID");

        jstring jstr = (jstring)java_env->CallObjectMethod(app->activity->clazz, method_id);

        const char* _str = java_env->GetStringUTFChars(jstr, NULL);
        std::string str(_str);
        java_env->ReleaseStringUTFChars(jstr, _str);

        jni_return = java_vm->DetachCurrentThread();
        if (jni_return != JNI_OK)
            throw std::runtime_error("Could not detach from thread");

        
        return str;
    }

    const std::vector<DevVIDPID> AIRSPY_VIDPIDS = {
        { 0x1d50, 0x60a1 }
    };

    const std::vector<DevVIDPID> AIRSPYHF_VIDPIDS = {
        { 0x03EB, 0x800C }
    };

    const std::vector<DevVIDPID> HACKRF_VIDPIDS = {
        { 0x1d50, 0x604b },
        { 0x1d50, 0x6089 },
        { 0x1d50, 0xcc15 }
    };

    const std::vector<DevVIDPID> HYDRASDR_VIDPIDS = {
        { 0x1d50, 0x60a1 },
        { 0x38af, 0x0001 }
    };

    const std::vector<DevVIDPID> RTL_SDR_VIDPIDS = {
        { 0x0bda, 0x2832 },
        { 0x0bda, 0x2838 },
        { 0x0413, 0x6680 },
        { 0x0413, 0x6f0f },
        { 0x0458, 0x707f },
        { 0x0ccd, 0x00a9 },
        { 0x0ccd, 0x00b3 },
        { 0x0ccd, 0x00b4 },
        { 0x0ccd, 0x00b5 },
        { 0x0ccd, 0x00b7 },
        { 0x0ccd, 0x00b8 },
        { 0x0ccd, 0x00b9 },
        { 0x0ccd, 0x00c0 },
        { 0x0ccd, 0x00c6 },
        { 0x0ccd, 0x00d3 },
        { 0x0ccd, 0x00d7 },
        { 0x0ccd, 0x00e0 },
        { 0x1554, 0x5020 },
        { 0x15f4, 0x0131 },
        { 0x15f4, 0x0133 },
        { 0x185b, 0x0620 },
        { 0x185b, 0x0650 },
        { 0x185b, 0x0680 },
        { 0x1b80, 0xd393 },
        { 0x1b80, 0xd394 },
        { 0x1b80, 0xd395 },
        { 0x1b80, 0xd397 },
        { 0x1b80, 0xd398 },
        { 0x1b80, 0xd39d },
        { 0x1b80, 0xd3a4 },
        { 0x1b80, 0xd3a8 },
        { 0x1b80, 0xd3af },
        { 0x1b80, 0xd3b0 },
        { 0x1d19, 0x1101 },
        { 0x1d19, 0x1102 },
        { 0x1d19, 0x1103 },
        { 0x1d19, 0x1104 },
        { 0x1f4d, 0xa803 },
        { 0x1f4d, 0xb803 },
        { 0x1f4d, 0xc803 },
        { 0x1f4d, 0xd286 },
        { 0x1f4d, 0xd803 }
    };
}

extern "C" {
    void android_main(struct android_app* app) {
        // Save app instance
        app->onAppCmd = backend::handleAppCmd;
        app->onInputEvent = backend::handleInputEvent;
        backend::app = app;

        // Check if this is the first time we run or not
        if (backend::initialized) {
            flog::warn("android_main called again");
            backend::doPartialInit();
            backend::pauseRendering = false;
            backend::renderLoop();
            return;
        }
        backend::initialized = true;

        // Grab files dir
        std::string appdir = backend::getAppFilesDir();

        // Call main
        char* rootpath = new char[appdir.size() + 1];
        strcpy(rootpath, appdir.c_str());
        char* dummy[] = { "", "-r", rootpath };
        sdrpp_main(3, dummy);
    }
}

#ifdef __ANDROID__
namespace backend {
    static std::function<void(std::string)> g_openCallback = nullptr;
    static std::function<void(std::string)> g_saveCallback = nullptr;
    static std::mutex g_dialogMtx;
    static std::vector<std::function<void()>> g_pendingDialogResults;

    // 在 MainActivity 实例上调用 Java 方法。
    // 注意：不能在原生线程里用 FindClass 查找应用自己的类（原生线程只能使用系统类加载器，
    // 会抛出 ClassNotFoundException 导致闪退），所以通过 MainActivity 对象转发给 Java 端。
    template <typename F>
    static void withActivity(F fn) {
        JavaVM* java_vm = app->activity->vm;
        JNIEnv* java_env = NULL;
        bool attached = false;
        jint r = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
        if (r == JNI_EDETACHED) {
            if (java_vm->AttachCurrentThread(&java_env, NULL) != JNI_OK) { return; }
            attached = true;
        }
        else if (r != JNI_OK) {
            return;
        }

        jclass cls = java_env->GetObjectClass(app->activity->clazz);
        if (cls) {
            fn(java_env, app->activity->clazz, cls);
            java_env->DeleteLocalRef(cls);
        }
        if (java_env->ExceptionCheck()) {
            java_env->ExceptionDescribe();
            java_env->ExceptionClear();
        }
        if (attached) { java_vm->DetachCurrentThread(); }
    }

    void openFileDialog(std::function<void(std::string)> callback) {
        {
            std::lock_guard<std::mutex> lck(g_dialogMtx);
            g_openCallback = callback;
        }
        withActivity([](JNIEnv* env, jobject activity, jclass cls) {
            jmethodID method = env->GetMethodID(cls, "openFilePicker", "()V");
            if (method) { env->CallVoidMethod(activity, method); }
        });
    }

    void saveFileDialog(std::string cacheFilePath, std::string defaultName, std::function<void(std::string)> callback) {
        {
            std::lock_guard<std::mutex> lck(g_dialogMtx);
            g_saveCallback = callback;
        }
        withActivity([&](JNIEnv* env, jobject activity, jclass cls) {
            jmethodID method = env->GetMethodID(cls, "saveFilePicker", "(Ljava/lang/String;Ljava/lang/String;)V");
            if (method) {
                jstring jPath = env->NewStringUTF(cacheFilePath.c_str());
                jstring jName = env->NewStringUTF(defaultName.c_str());
                env->CallVoidMethod(activity, method, jPath, jName);
                env->DeleteLocalRef(jPath);
                env->DeleteLocalRef(jName);
            }
        });
    }

    // 文件选择结果由 Java 的界面线程回调进来；真正的处理（导入书签会修改界面正在使用的数据）
    // 放到渲染线程中执行，避免和界面绘制同时访问数据导致崩溃
    void ProcessPendingDialogResults() {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lck(g_dialogMtx);
            pending.swap(g_pendingDialogResults);
        }
        for (auto& fn : pending) {
            // 回调里（例如导入书签）抛出的异常不能传出渲染线程，否则会直接闪退
            try { fn(); }
            catch (const std::exception& e) { flog::error("File dialog callback failed: {0}", e.what()); }
            catch (...) { flog::error("File dialog callback failed with unknown exception"); }
        }
    }

    // ---- 系统剪贴板 ----
    // ImGui 的输入框复制/剪切/粘贴通过这两个函数读写剪贴板，实际由 MainActivity（Java 端）完成
    const char* androidGetClipboard(void* userData) {
        static std::string text;
        text.clear();
        withActivity([&](JNIEnv* env, jobject activity, jclass cls) {
            jmethodID method = env->GetMethodID(cls, "getClipboardText", "()Ljava/lang/String;");
            if (!method) { return; }
            jstring result = (jstring)env->CallObjectMethod(activity, method);
            if (result) {
                const char* chars = env->GetStringUTFChars(result, nullptr);
                if (chars) { text = chars; env->ReleaseStringUTFChars(result, chars); }
                env->DeleteLocalRef(result);
            }
        });
        return text.c_str();
    }

    void androidSetClipboard(void* userData, const char* text) {
        if (!text) { return; }
        withActivity([&](JNIEnv* env, jobject activity, jclass cls) {
            jmethodID method = env->GetMethodID(cls, "setClipboardText", "(Ljava/lang/String;)V");
            if (!method) { return; }
            jstring jText = env->NewStringUTF(text);
            env->CallVoidMethod(activity, method, jText);
            env->DeleteLocalRef(jText);
        });
    }

    // ---- 输入框编辑工具栏 ----
    // 软键盘弹出时，Java 端在键盘上方显示一排原生按钮（全选/复制/剪切/粘贴/删除）。
    // 用原生按钮而不是 ImGui 控件，是因为点击 ImGui 里的其他控件会让输入框失去焦点。
    // 按钮点击后通过 JNI 回调把操作放进队列，渲染线程每帧取出一个交给当前输入框处理。
    static std::mutex g_editMtx;
    static std::vector<int> g_editActions;

    void updateEditBarLabels() {
        // 文字走界面语言翻译；语言文件里的原文键是下面这几个中文词
        std::string labels[5] = { i18n::tr("全选"), i18n::tr("复制"), i18n::tr("剪切"), i18n::tr("粘贴"), i18n::tr("删除") };
        withActivity([&](JNIEnv* env, jobject activity, jclass cls) {
            jmethodID method = env->GetMethodID(cls, "setEditBarLabels", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (!method) { return; }
            jstring j[5];
            for (int i = 0; i < 5; i++) { j[i] = env->NewStringUTF(labels[i].c_str()); }
            env->CallVoidMethod(activity, method, j[0], j[1], j[2], j[3], j[4]);
            for (int i = 0; i < 5; i++) { env->DeleteLocalRef(j[i]); }
        });
    }

    void processEditActions() {
        int action = 0;
        {
            std::lock_guard<std::mutex> lck(g_editMtx);
            if (!g_editActions.empty()) {
                action = g_editActions.front();
                g_editActions.erase(g_editActions.begin());
            }
        }
        if (action != 0) { ImGui::TextInputAction(action); }
    }

    void pushEditAction(int action) {
        std::lock_guard<std::mutex> lck(g_editMtx);
        if (g_editActions.size() < 8) { g_editActions.push_back(action); }
    }

    static std::string jstringToStd(JNIEnv* env, jstring str) {
        if (!str) { return ""; }
        const char* chars = env->GetStringUTFChars(str, nullptr);
        std::string result(chars ? chars : "");
        if (chars) { env->ReleaseStringUTFChars(str, chars); }
        return result;
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_sdrpp_sdrpp_SdrFilePicker_nativeOnResultOpen(JNIEnv* env, jobject thiz, jstring path) {
    std::string result = backend::jstringToStd(env, path);
    std::lock_guard<std::mutex> lck(backend::g_dialogMtx);
    if (backend::g_openCallback) {
        auto cb = backend::g_openCallback;
        backend::g_openCallback = nullptr;
        backend::g_pendingDialogResults.push_back([cb, result]() { cb(result); });
    }
}

// 输入框编辑工具栏的按钮：1 全选，2 复制，3 剪切，4 粘贴，5 删除
extern "C" JNIEXPORT void JNICALL Java_org_sdrpp_sdrpp_MainActivity_nativeEditAction(JNIEnv* env, jobject thiz, jint action) {
    backend::pushEditAction((int)action);
}

extern "C" JNIEXPORT void JNICALL Java_org_sdrpp_sdrpp_SdrFilePicker_nativeOnResultSave(JNIEnv* env, jobject thiz, jstring result) {
    std::string res = backend::jstringToStd(env, result);
    std::lock_guard<std::mutex> lck(backend::g_dialogMtx);
    if (backend::g_saveCallback) {
        auto cb = backend::g_saveCallback;
        backend::g_saveCallback = nullptr;
        backend::g_pendingDialogResults.push_back([cb, res]() { cb(res); });
    }
}
#endif

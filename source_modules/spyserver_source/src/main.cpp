#include <spyserver_client.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <utils/i18n.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <gui/smgui.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spyserver_source",
    /* Description:     */ "SpyServer source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

const char* deviceTypesStr[] = {
    "未知",
    "Airspy One",
    "Airspy HF+",
    "RTL-SDR"
};

const char* streamFormatStr = "UInt8\0"
                              "Int16\0"
                              "Float32\0";

const SpyServerStreamFormat streamFormats[] = {
    SPYSERVER_STREAM_FORMAT_UINT8,
    SPYSERVER_STREAM_FORMAT_INT16,
    SPYSERVER_STREAM_FORMAT_FLOAT
};

const int streamFormatsBitCount[] = {
    8,
    16,
    32
};

ConfigManager config;

class SpyServerSourceModule : public ModuleManager::Instance {
public:
    SpyServerSourceModule(std::string name) {
        this->name = name;

        config.acquire();
        std::string host = config.conf["hostname"];
        port = config.conf["port"];
        config.release();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        snprintf(hostname, sizeof(hostname), "%s", host.c_str());

        sigpath::sourceManager.registerSource("SpyServer", &handler);
    }

    ~SpyServerSourceModule() {
        stop(this);
        // 等待后台连接线程结束（连接有超时，不会一直等下去）
        if (connectThread.joinable()) { connectThread.join(); }
        sigpath::sourceManager.unregisterSource("SpyServer");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        gui::mainWindow.playButtonLocked = !(_this->client && _this->client->isOpen());
        flog::info("SpyServerSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        gui::mainWindow.playButtonLocked = false;
        flog::info("SpyServerSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        if (_this->running) { return; }
        
        // 没有连接时不能开始（连接在后台线程里进行，开始按钮在连接成功前是锁定的）
        if (!_this->client || !_this->client->isOpen()) { return; }

        int srvBits = streamFormatsBitCount[_this->iqType];
        _this->client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[_this->iqType]);
        _this->client->setSetting(SPYSERVER_SETTING_IQ_DECIMATION, _this->srId + _this->client->devInfo.MinimumIQDecimation);
        _this->client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, _this->freq);
        _this->client->setSetting(SPYSERVER_SETTING_STREAMING_MODE, SPYSERVER_STREAM_MODE_IQ_ONLY);
        _this->client->setSetting(SPYSERVER_SETTING_GAIN, _this->gain);
        _this->client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, _this->client->computeDigitalGain(srvBits, _this->gain, _this->srId + _this->client->devInfo.MinimumIQDecimation));
        _this->client->startStream();

        _this->running = true;
        flog::info("SpyServerSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        if (!_this->running) { return; }

        _this->client->stopStream();

        _this->running = false;
        flog::info("SpyServerSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        if (_this->running) {
            _this->client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, freq);
        }
        _this->freq = freq;
        flog::info("SpyServerSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;

        // 后台连接线程完成后，在界面线程里应用结果
        _this->finishConnect();

        bool connected = (_this->client && _this->client->isOpen());
        bool connecting = _this->connecting;
        gui::mainWindow.playButtonLocked = !connected;

        if (connected || connecting) { SmGui::BeginDisabled(); }
        if (SmGui::InputText(CONCAT("##_spyserver_srv_host_", _this->name), _this->hostname, 1023)) {
            config.acquire();
            config.conf["hostname"] = _this->hostname;
            config.release(true);
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_spyserver_srv_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf["port"] = _this->port;
            config.release(true);
        }
        if (connected || connecting) { SmGui::EndDisabled(); }

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (connecting) {
            SmGui::BeginDisabled();
            SmGui::Button("正在连接...##spyserver_source");
            SmGui::EndDisabled();
        }
        else if (!connected && SmGui::Button("连接##spyserver_source")) {
            _this->tryConnect();
        }
        else if (connected && SmGui::Button("断开##spyserver_source")) {
            _this->client->close();
        }
        if (_this->running) { SmGui::EndDisabled(); }


        if (connected) {
            if (_this->running) { style::beginDisabled(); }
            SmGui::LeftLabel("采样率");
            SmGui::FillWidth();
            if (SmGui::Combo("##spyserver_source_sr", &_this->srId, _this->sampleRatesTxt.c_str())) {
                _this->sampleRate = _this->sampleRates[_this->srId];
                core::setInputSampleRate(_this->sampleRate);
                config.acquire();
                config.conf["devices"][_this->devRef]["sampleRateId"] = _this->srId;
                config.release(true);
            }
            if (_this->running) { style::endDisabled(); }

            SmGui::LeftLabel("采样位深");
            SmGui::FillWidth();
            if (SmGui::Combo("##spyserver_source_type", &_this->iqType, streamFormatStr)) {
                int srvBits = streamFormatsBitCount[_this->iqType];
                _this->client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[_this->iqType]);
                _this->client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, _this->client->computeDigitalGain(srvBits, _this->gain, _this->srId + _this->client->devInfo.MinimumIQDecimation));

                config.acquire();
                config.conf["devices"][_this->devRef]["sampleBitDepthId"] = _this->iqType;
                config.release(true);
            }

            if (_this->client->devInfo.MaximumGainIndex) {
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_gain", (int*)&_this->gain, 0, _this->client->devInfo.MaximumGainIndex)) {
                    int srvBits = streamFormatsBitCount[_this->iqType];
                    _this->client->setSetting(SPYSERVER_SETTING_GAIN, _this->gain);
                    _this->client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, _this->client->computeDigitalGain(srvBits, _this->gain, _this->srId + _this->client->devInfo.MinimumIQDecimation));
                    config.acquire();
                    config.conf["devices"][_this->devRef]["gainId"] = _this->gain;
                    config.release(true);
                }
            }

            SmGui::Text("状态:");
            SmGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "已连接 (%s)", i18n::tr(deviceTypesStr[_this->client->devInfo.DeviceType]));
        }
        else {
            SmGui::Text("状态:");
            SmGui::SameLine();
            SmGui::Text("未连接");
        }

        // 连接错误提示：直接显示在菜单里（弹窗在手机上容易被软键盘遮挡，远程界面模式下也看不到）
        if (!_this->errorMsg.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _this->errorMsg.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // 连接放在后台线程里做：域名解析、连接超时（最长约 8 秒/地址）和等待设备信息都可能阻塞，
    // 在界面线程里做会让整个程序卡住（Windows 显示“无响应”，Android 可能被系统判定为无响应而杀掉）。
    void tryConnect() {
        if (connecting) { return; }
        if (connectThread.joinable()) { connectThread.join(); }
        if (client) { client.reset(); }
        errorMsg = "";
        connecting = true;
        connectDone = false;

        std::string host = hostname;
        int portNum = port;
        connectThread = std::thread([this, host, portNum]() {
            spyserver::SpyServerClient c;
            std::string err;
            try {
                c = spyserver::connect(host, portNum, &stream);
                if (!c) {
                    err = "连接失败";
                }
                else if (!c->waitForDevInfo(3000)) {
                    flog::error("SpyServer didn't respond with device information");
                    err = "服务器未返回设备信息（超时），请确认地址、端口以及服务器是否正常运行";
                    c.reset();
                }
            }
            catch (const std::exception& e) {
                flog::error("Could not connect to spyserver {}", e.what());
                err = std::string(i18n::tr("连接失败: ")) + e.what();
                c.reset();
            }
            {
                std::lock_guard<std::mutex> lck(connectMtx);
                pendingClient = std::move(c);
                pendingError = err;
            }
            connectDone = true;
        });
    }

    // 在界面线程里调用：取走后台连接的结果
    void finishConnect() {
        if (!connectDone.exchange(false)) { return; }
        if (connectThread.joinable()) { connectThread.join(); }

        spyserver::SpyServerClient c;
        std::string err;
        {
            std::lock_guard<std::mutex> lck(connectMtx);
            c = std::move(pendingClient);
            pendingClient = nullptr;
            err = pendingError;
        }
        connecting = false;

        if (!c) {
            errorMsg = i18n::tr(err.c_str());
            return;
        }
        client = std::move(c);

        char buf[1024];
        sprintf(buf, "%s [%08X]", deviceTypesStr[client->devInfo.DeviceType], client->devInfo.DeviceSerial);
        devRef = std::string(buf);

        config.acquire();
        if (!config.conf["devices"].contains(devRef)) {
            config.conf["devices"][devRef]["sampleRateId"] = 0;
            config.conf["devices"][devRef]["sampleBitDepthId"] = 1;
            config.conf["devices"][devRef]["gainId"] = 0;
        }
        srId = config.conf["devices"][devRef]["sampleRateId"];
        iqType = config.conf["devices"][devRef]["sampleBitDepthId"];
        gain = config.conf["devices"][devRef]["gainId"];
        config.release(true);

        gain = std::clamp<int>(gain, 0, client->devInfo.MaximumGainIndex);

        // Refresh sample rates
        sampleRates.clear();
        sampleRatesTxt.clear();
        for (int i = client->devInfo.MinimumIQDecimation; i <= client->devInfo.DecimationStageCount; i++) {
            double sr = (double)client->devInfo.MaximumSampleRate / ((double)(1 << i));
            sampleRates.push_back(sr);
            sampleRatesTxt += getBandwdithScaled(sr);
            sampleRatesTxt += '\0';
        }

        srId = std::clamp<int>(srId, 0, sampleRates.size() - 1);

        sampleRate = sampleRates[srId];
        core::setInputSampleRate(sampleRate);
        flog::info("Connected to server");
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double sampleRate = 1000000;
    double freq;

    char hostname[1024];
    int port = 5555;
    int iqType = 0;

    int srId = 0;
    std::vector<double> sampleRates;
    std::string sampleRatesTxt;

    uint32_t gain = 0;

    std::string devRef = "";
    std::string errorMsg = ""; // 显示在菜单中的连接错误信息

    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;

    spyserver::SpyServerClient client;

    // 后台连接
    std::thread connectThread;
    std::atomic<bool> connecting{ false };
    std::atomic<bool> connectDone{ false };
    std::mutex connectMtx;
    spyserver::SpyServerClient pendingClient;
    std::string pendingError;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["hostname"] = "localhost";
    def["port"] = 5555;
    def["devices"] = json::object();
    config.setPath(core::args["root"].s() + "/spyserver_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check config in case a user has a very old version
    config.acquire();
    bool corrected = false;
    if (!config.conf.contains("hostname") || !config.conf.contains("port") || !config.conf.contains("devices")) {
        config.conf = def;
        corrected = true;
    }
    config.release(corrected);
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpyServerSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpyServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
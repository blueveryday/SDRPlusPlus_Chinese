#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // must come before anything pulls in windows.h, otherwise winsock.h clashes with winsock2.h
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <utils/flog.h>
#include <utils/i18n.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <utils/host_history.h>
#include <atomic>
#include <thread>
#include <vector>
#include <complex>
#include <cstring>
#include <ctime>
#include "kiwisdr_client.h"
#include "kiwisdr_map.h"
#include "kiwi_util.h"
#include "kiwi_widgets.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "kiwisdr_source",
    /* Description:     */ "KiwiSDR WebSDR source module for SDR++",
    /* Author:          */ "san (ported)",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

struct KiwiSDRSourceModule : public ModuleManager::Instance {
    KiwiSDRSourceModule(std::string name) : selector(core::args["root"].s(), &config, "kiwisdr_map") {
        this->name = name;

        config.acquire();
        if (config.conf.contains("kiwisdr_site")) {
            // 配置里保存的是 "主机:端口"（旧版本也可能是 http://主机:端口/ 这样的网址），拆成地址和端口两个输入框
            std::string site = config.conf["kiwisdr_site"];
            setSite(site);
        }
        if (config.conf.contains("kiwisdr_loc")) {
            kiwisdrLoc = config.conf["kiwisdr_loc"];
        }
        config.release(false);

        hostHistory.load("kiwisdr");
        kiwiSdrClient.init(siteString());

        kiwiSdrClient.onConnected = [this]() {
            connected = true;
            historyPending = true;  // 连接成功：到界面线程里记入地址历史（这里是网络线程）
            tune(lastTuneFrequency, this);
        };
        kiwiSdrClient.onDisconnected = [this]() {
            connected = false;
            feederRun = false;
            // Only an unexpected disconnect gets here with running == true (stop() clears it first).
            if (running.exchange(false)) {
                // If we failed so fast that setPlayState(true) has not finished yet, wait for it
                for (int i = 0; i < 20 && !gui::mainWindow.isPlaying(); i++) { kiwiSleepMicros(50000); }
                gui::mainWindow.setPlayState(false);
            }
        };

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        sigpath::sourceManager.registerSource("KiwiSDR", &handler);
    }

    ~KiwiSDRSourceModule() {
        stop(this);
        running = false;
        feederRun = false;
        if (kiwiSdrClient.running) { kiwiSdrClient.stop(); }
        if (feeder.joinable()) { feeder.join(); }
        sigpath::sourceManager.unregisterSource("KiwiSDR");
    }

    void postInit() {}

    void enable() { enabled = true; }

    void disable() { enabled = false; }

    bool isEnabled() { return enabled; }

private:
    static void menuSelected(void* ctx) {
        core::setInputSampleRate(12000); // KiwiSDR IQ mode is fixed at 12 kHz
        flog::info("KiwiSDRSourceModule '{0}': Menu Select!", ((KiwiSDRSourceModule*)ctx)->name);
    }

    static void menuDeselected(void* ctx) {
        flog::info("KiwiSDRSourceModule '{0}': Menu Deselect!", ((KiwiSDRSourceModule*)ctx)->name);
    }

    static void start(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        if (_this->running) { return; }

        // A previous session may still be winding down (e.g. after a dropped connection)
        _this->feederRun = false;
        if (_this->kiwiSdrClient.running) { _this->kiwiSdrClient.stop(); }
        if (_this->feeder.joinable()) { _this->feeder.join(); }

        _this->running = true;
        _this->kiwiSdrClient.init(_this->siteString());
        _this->kiwiSdrClient.start();

        sigpath::iqFrontEnd.setCurrentStreamTime(0); // not started yet
        _this->timeSet = false;
        _this->feederRun = true;
        _this->feeder = std::thread([_this]() { _this->feederLoop(); });

        flog::info("KiwiSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->feederRun = false;
        _this->kiwiSdrClient.stop();
        if (_this->feeder.joinable()) { _this->feeder.join(); }
        _this->connected = false;
        sigpath::iqFrontEnd.setCurrentStreamTime(0); // back to realtime
        flog::info("KiwiSDRSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        _this->lastTuneFrequency = freq;
        if (_this->running && _this->connected) {
            _this->kiwiSdrClient.tune(freq, KiwiSDRClient::TUNE_IQ);
        }
        flog::info("KiwiSDRSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    // Pace the 12 kHz IQ data coming from the network into the SDR++ stream, 200 samples at a time (60 Hz)
    void feederLoop() {
        double nextSend = 0;
        while (feederRun) {
            kiwiSdrClient.iqDataLock.lock();
            auto bufsize = kiwiSdrClient.iqData.size();
            kiwiSdrClient.iqDataLock.unlock();
            double now = (double)kiwiNowMillis();
            if (nextSend == 0) {
                if (bufsize < 200) {
                    kiwiSleepMicros(16000); // waiting for the initial batch
                    continue;
                }
                nextSend = now;
            }
            else {
                double sleepTime = (nextSend - now) * 1000;
                if (sleepTime > 0) { kiwiSleepMicros((long long)sleepTime); }
            }

            std::vector<std::complex<float>> toSend;
            int remaining = 0;
            kiwiSdrClient.iqDataLock.lock();
            if (kiwiSdrClient.iqData.size() >= 200) {
                toSend.assign(kiwiSdrClient.iqData.begin(), kiwiSdrClient.iqData.begin() + 200);
                kiwiSdrClient.iqData.erase(kiwiSdrClient.iqData.begin(), kiwiSdrClient.iqData.begin() + 200);
                remaining = (int)kiwiSdrClient.iqData.size();
            }
            kiwiSdrClient.iqDataLock.unlock();

            // Buffer too full -> catch up a bit faster
            nextSend += (remaining > kiwiSdrClient.NETWORK_BUFFER_SIZE) ? (1000.0 / 120.0) : (1000.0 / 60.0);

            if (!toSend.empty()) {
                memcpy(stream.writeBuf, toSend.data(), toSend.size() * sizeof(dsp::complex_t));
                stream.swap((int)toSend.size());
            }
            else {
                nextSend = 0; // underflow, re-buffer
            }

            // Capture time of the sample being sent: now, minus what is still queued, minus a safety margin.
            long long queuedMs = (long long)remaining * 1000 / kiwiSdrClient.IQDATA_FREQUENCY;
            long long newStreamTime = kiwiNowMillis() - queuedMs - 500;
            if (!timeSet) {
                sigpath::iqFrontEnd.setCurrentStreamTime(newStreamTime);
                timeSet = true;
            }
            else if (sigpath::iqFrontEnd.getCurrentStreamTime() < newStreamTime) {
                sigpath::iqFrontEnd.setCurrentStreamTime(newStreamTime); // only ever moves forward
            }
        }
    }

    static void menuHandler(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;

        if (!core::args["server"].b()) {
            // local UI only: the map popup uses plain ImGui
            ImGui::BeginDisabled(_this->running);
            if (doFingerButton("在地图上选择...")) {
                _this->selector.openPopup();
            }
            ImGui::EndDisabled();

            _this->selector.drawPopup([_this](const std::string& hostPort, const std::string& loc) {
                _this->setSite(hostPort);
                _this->kiwisdrLoc = loc;
                _this->saveSite();
                _this->kiwiSdrClient.init(_this->siteString());
            });
        }

        // 连接成功后（网络线程里设的标记），在界面线程里把地址记入历史
        if (_this->historyPending.exchange(false)) {
            _this->hostHistory.add(_this->hostname, _this->port);
        }

        if (_this->running) { SmGui::BeginDisabled(); }
        // 地址历史：从下拉框选一条，同时填好地址和端口
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_kiwisdr_hist_", _this->name), &_this->hostHistoryId, _this->hostHistory.items())) {
            if (_this->hostHistory.select(_this->hostHistoryId, _this->hostname, sizeof(_this->hostname), &_this->port) == HostHistory::SELECTED) {
                _this->kiwisdrLoc.clear();
                _this->saveSite();
                _this->kiwiSdrClient.init(_this->siteString());
            }
            _this->hostHistoryId = 0;
        }
        // 地址和端口两个输入框（和 SpyServer 一样）
        if (SmGui::InputText(CONCAT("##_kiwisdr_host_", _this->name), _this->hostname, sizeof(_this->hostname))) {
            _this->kiwisdrLoc.clear(); // manual edit: the stored location no longer applies
            _this->saveSite();
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_kiwisdr_port_", _this->name), &_this->port, 0, 0)) {
            _this->port = std::clamp<int>(_this->port, 1, 65535);
            _this->kiwisdrLoc.clear();
            _this->saveSite();
        }
        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::Text((std::string(i18n::tr("位置: ")) + _this->kiwisdrLoc).c_str());
        SmGui::Text((std::string(i18n::tr("状态: ")) + std::string(_this->kiwiSdrClient.connectionStatus)).c_str());

        std::time_t t = (std::time_t)(sigpath::iqFrontEnd.getCurrentStreamTime() / 1000);
        std::tm* tmm = std::localtime(&t);
        char streamTime[64] = "-";
        if (tmm) { strftime(streamTime, sizeof(streamTime), "%Y-%m-%d %H:%M:%S", tmm); }
        SmGui::Text((std::string(i18n::tr("流时间: ")) + std::string(streamTime)).c_str());
    }

    std::string name;
    bool enabled = true;
    std::atomic<bool> running{ false };
    std::atomic<bool> feederRun{ false };
    std::atomic<bool> connected{ false };
    bool timeSet = false;
    double lastTuneFrequency = 14100000.0;

    char hostname[256] = "kiwi.example.com";
    int port = 8073;
    std::string kiwisdrLoc;

    HostHistory hostHistory;
    int hostHistoryId = 0;
    std::atomic<bool> historyPending{ false };

    // 地址 + 端口拼成客户端要的 "主机:端口"（IPv6 地址加方括号）
    std::string siteString() const {
        std::string h = hostname;
        if (h.find(':') != std::string::npos) { h = "[" + h + "]"; }
        return h + ":" + std::to_string(port);
    }

    // 把 "主机:端口" 或 http://主机:端口/ 拆进地址和端口两个变量；解析失败就原样当作地址
    void setSite(const std::string& site) {
        KiwiEndpoint ep = kiwiParseSite(site);
        if (ep.error.empty() && !ep.host.empty() && ep.port >= 1 && ep.port <= 65535) {
            snprintf(hostname, sizeof(hostname), "%s", ep.host.c_str());
            port = ep.port;
        }
        else {
            snprintf(hostname, sizeof(hostname), "%s", site.c_str());
        }
    }

    void saveSite() {
        config.acquire();
        config.conf["kiwisdr_site"] = siteString();
        config.conf["kiwisdr_loc"] = kiwisdrLoc;
        config.release(true);
    }

    KiwiSDRClient kiwiSdrClient;
    KiwiSDRMapSelector selector;
    std::thread feeder;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/kiwisdr_source_config.json");
    config.load(json({}));
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new KiwiSDRSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (KiwiSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}

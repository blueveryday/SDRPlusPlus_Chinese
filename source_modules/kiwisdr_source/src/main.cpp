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
            std::string site = config.conf["kiwisdr_site"];
            snprintf(siteBuf, sizeof(siteBuf), "%s", site.c_str());
        }
        if (config.conf.contains("kiwisdr_loc")) {
            kiwisdrLoc = config.conf["kiwisdr_loc"];
        }
        config.release(false);

        kiwiSdrClient.init(siteBuf);

        kiwiSdrClient.onConnected = [this]() {
            connected = true;
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
        _this->kiwiSdrClient.init(_this->siteBuf);
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
                snprintf(_this->siteBuf, sizeof(_this->siteBuf), "%s", hostPort.c_str());
                _this->kiwisdrLoc = loc;
                config.acquire();
                config.conf["kiwisdr_site"] = std::string(_this->siteBuf);
                config.conf["kiwisdr_loc"] = _this->kiwisdrLoc;
                config.release(true);
                _this->kiwiSdrClient.init(_this->siteBuf);
            });
        }

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("KiwiSDR 地址");
        SmGui::FillWidth();
        if (SmGui::InputText(CONCAT("##_kiwisdr_site_", _this->name), _this->siteBuf, sizeof(_this->siteBuf))) {
            _this->kiwisdrLoc.clear(); // manual edit: the stored location no longer applies
            config.acquire();
            config.conf["kiwisdr_site"] = std::string(_this->siteBuf);
            config.conf["kiwisdr_loc"] = _this->kiwisdrLoc;
            config.release(true);
        }
        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::Text("格式: 主机:端口 或 http://主机:端口/");
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

    char siteBuf[256] = "kiwi.example.com:8073";
    std::string kiwisdrLoc;

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

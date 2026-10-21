#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <dsp/types.h>
#include <complex>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <utils/flog.h>
#include <atomic>
#include "kiwi_util.h"
#include "websock.h"

struct KiwiEndpoint {
    std::string host;
    int port = 0;
    std::string error;
};

inline KiwiEndpoint kiwiParseSite(std::string s) {
    KiwiEndpoint ep;
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

    std::string scheme;
    auto schemePos = s.find("://");
    if (schemePos != std::string::npos) {
        scheme = s.substr(0, schemePos);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        s = s.substr(schemePos + 3);
    }
    auto cut = s.find_first_of("/?#");
    if (cut != std::string::npos) { s.erase(cut); }
    auto at = s.rfind('@');
    if (at != std::string::npos) { s = s.substr(at + 1); }

    std::string portStr;
    if (!s.empty() && s[0] == '[') {
        auto rb = s.find(']');
        if (rb == std::string::npos) { ep.error = "address is invalid"; return ep; }
        ep.host = s.substr(1, rb - 1);
        auto rest = s.substr(rb + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') { ep.error = "address is invalid"; return ep; }
            portStr = rest.substr(1);
        }
    }
    else if (std::count(s.begin(), s.end(), ':') > 1) {
        ep.host = s;
    }
    else {
        auto colon = s.find(':');
        if (colon != std::string::npos) {
            ep.host = s.substr(0, colon);
            portStr = s.substr(colon + 1);
        }
        else {
            ep.host = s;
        }
    }

    if (ep.host.empty()) { ep.error = "host name is empty"; return ep; }
    if (!scheme.empty() && scheme != "http" && scheme != "ws" && scheme != "https" && scheme != "wss") {
        ep.error = "unsupported scheme: " + scheme;
        return ep;
    }

    if (!portStr.empty()) {
        if (portStr.size() > 5 || !std::all_of(portStr.begin(), portStr.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            ep.error = "port is invalid";
            return ep;
        }
        ep.port = std::atoi(portStr.c_str());
        if (ep.port < 1 || ep.port > 65535) { ep.error = "port is invalid"; return ep; }
    }
    else if (scheme == "https" || scheme == "wss") {
        ep.error = "https/wss (TLS) is not supported, use http://host:port";
        return ep;
    }
    else {
        ep.port = 8073;
    }
    return ep;
}

struct KiwiSDRClient {
    net::websock::WSClient wsClient;
    std::string hostPort;
    bool connected = false;
    char connectionStatus[256];
    int64_t lastPing = 0;
    std::atomic<bool> running{ false };
    std::vector<int64_t> times;

    std::string identUser = "kiwisdr";

    std::function<void()> onConnected = []() {};
    std::function<void()> onDisconnected = []() {};
    std::vector<std::complex<float>> iqData;
    std::mutex iqDataLock;


    virtual ~KiwiSDRClient() {
        running = false;
        flog::info("KiwiSDRClient: destructor");
        running = false;
    }

    int IQDATA_FREQUENCY = 12000;
    int NETWORK_BUFFER_SECONDS = 2;
    int NETWORK_BUFFER_SIZE = NETWORK_BUFFER_SECONDS * IQDATA_FREQUENCY;

    void init(const std::string& hostport) {

        this->hostPort = hostport;
        strcpy(connectionStatus, "Not connected");
        static std::atomic_int outCount;

        outCount = 0;

        wsClient.onDisconnected = [&]() {
            connected = false;
            this->onDisconnected();
            snprintf(connectionStatus, sizeof connectionStatus, "Disconnected");
        };

        wsClient.onConnected = [&]() {
            wsClient.sendString("SET auth t=kiwi p=#");
            if (!identUser.empty()) {
                wsClient.sendString("SET ident_user=" + identUser);
                flog::info("KiwiSDRClient: sent SET ident_user={}", identUser);
            }
            wsClient.sendString("SET AR OK in=" + std::to_string(IQDATA_FREQUENCY) + " out=48000");
            wsClient.sendString("SERVER DE CLIENT sdr++brown SND");
            wsClient.sendString("SET compression=0");
            wsClient.sendString("SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 manGain=50");
            connected = true;
            if (this->onConnected) {
                onConnected();
            }
            strcpy(connectionStatus, "Connected, waiting data...");
        };

        wsClient.onTextMessage = [&](const std::string& msg) {
            flog::info("TEXT: {}", msg);

            std::string lower = msg;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });

            if (lower.find("ident") != std::string::npos ||
                lower.find("callsign") != std::string::npos ||
                lower.find("enter your name") != std::string::npos) {
                flog::warn("KiwiSDR server requires identification. Current ident_user='{}'.", identUser);
                snprintf(connectionStatus, sizeof connectionStatus,
                         "Server requires ID (sent: %s)", identUser.c_str());
            }
            if (lower.find("password") != std::string::npos &&
                lower.find("required") != std::string::npos) {
                flog::warn("KiwiSDR server requires a password (not supported by this client).");
                snprintf(connectionStatus, sizeof connectionStatus,
                         "Server requires password");
            }
            if (lower.find("too many") != std::string::npos ||
                lower.find("full") != std::string::npos ||
                lower.find("rejected") != std::string::npos) {
                flog::warn("KiwiSDR server rejected the connection: {}", msg);
                snprintf(connectionStatus, sizeof connectionStatus,
                         "Server rejected: too many users / full");
            }
        };

        wsClient.onBinaryMessage = [&](const std::string& msg) {
            std::string start = "???";
            if (msg.size() > 3) {
                start = msg.substr(0, 3);
            }
            if (start == "MSG") {
            }
            else if (start == "SND") {
                if ((outCount++) % 50 == 0) {
                }
                long long int ctm = kiwiNowMillis();
                times.emplace_back(ctm);
                int lastSecondCount = 0;
                for (int q = times.size() - 1; q >= 0; q--) {
                    if (times[q] < ctm - 1000) {
                        break;
                    }
                    lastSecondCount++;
                }
                while (!times.empty() && times.front() < ctm - 2000) {
                    times.erase(times.begin());
                }
                snprintf(connectionStatus, sizeof connectionStatus, "Receiving. %d KB/sec (%d)", (lastSecondCount * ((int)msg.size())) / 1024, lastSecondCount);
                int IQ_HEADER_SIZE = 20;
                int REAL_HEADER_SIZE = 10;
                if (currentModulation == TUNE_REAL && msg.size() == 1024 + REAL_HEADER_SIZE) {
                    auto scan = msg.data();
                    scan += REAL_HEADER_SIZE;

                    if (scan != msg.data() + REAL_HEADER_SIZE) {
                        flog::info("ERROR scan != msg.data() + REAL_HEADER_SIZE\n");
                        abort();
                    }

                    int16_t* ptr = (int16_t*)scan;
                    snprintf(connectionStatus, sizeof connectionStatus, "Storing real..");
                    iqDataLock.lock();
                    for (int z = 0; z < 512; z++) {
                        int16_t* iqsample = &ptr[z];
                        char* fourbytes = (char*)iqsample;
                        std::swap(fourbytes[0], fourbytes[1]);
                        iqData.emplace_back(iqsample[0] / 32767.0, 0);
                    }
                    while (iqData.size() > NETWORK_BUFFER_SIZE * 1.5) {
                        iqData.erase(iqData.begin(), iqData.begin() + 200);
                    }
                    iqDataLock.unlock();
                    snprintf(connectionStatus, sizeof connectionStatus, "Cont Recv. %d KB/sec (%d)", (lastSecondCount * ((int)msg.size())) / 1024, lastSecondCount);
                }
                if (currentModulation == TUNE_IQ && msg[3] == 0x08 && msg.size() == 2048 + IQ_HEADER_SIZE) {
                    auto scan = msg.data();
                    scan += 4;
                    auto sequence = *(int32_t*)scan;
                    scan += 4;
                    char one = *(char*)scan;
                    scan += 1;
                    auto info1 = *(int16_t*)scan;
                    scan += 2;
                    char zero = *(char*)scan;
                    scan += 1;
                    auto timestamp = *(int32_t*)scan;
                    scan += 4;
                    auto* arg2 = (int32_t*)scan;
                    scan += 4;

                    if (scan != msg.data() + IQ_HEADER_SIZE) {
                        flog::info("ERROR scan != msg.data() + IQ_HEADER_SIZE\n");
                        abort();
                    }

                    int16_t* ptr = (int16_t*)scan;
                    iqDataLock.lock();
                    for (int z = 0; z < 512; z++) {
                        int16_t* iqsample = &ptr[2 * z];
                        char* fourbytes = (char*)iqsample;
                        std::swap(fourbytes[0], fourbytes[1]);
                        std::swap(fourbytes[2], fourbytes[3]);
                        iqData.emplace_back(iqsample[0] / 32767.0, iqsample[1] / 32767.0);
                    }
                    while (iqData.size() > NETWORK_BUFFER_SIZE * 1.5) {
                        iqData.erase(iqData.begin(), iqData.begin() + 200);
                    }
                    iqDataLock.unlock();
                }
            }
            else {
                if (msg.size() >= 70) {
                    char buf[100];
                    for (int q = 3; q < 30; q++) {
                        snprintf(buf, sizeof buf,"%02x ", (unsigned char)msg[q]);
                        start += buf;
                    }
                    start += "... ";
                    for (int q = -20; q < 0; q++) {
                        snprintf(buf, sizeof buf,"%02x ", (unsigned char)msg[msg.size() - 1 + q]);
                        start += buf;
                    }
                }
            }
        };
        lastPing = kiwiNowMillis();
        wsClient.onEveryReceive = [&]() {
            if (kiwiNowMillis() - lastPing > 4000) {
                wsClient.sendString("SET keepalive");
                lastPing = kiwiNowMillis();
            }
        };
    }


    enum Modulation {
        TUNE_IQ = 1,
        TUNE_REAL = 2,
    };

    Modulation currentModulation = TUNE_IQ;

    void tune(double freq, Modulation mod) {
        char buf[1024];
        switch (mod) {
        case Modulation::TUNE_IQ:
            currentModulation = mod;
            snprintf(buf, sizeof buf, "SET mod=iq low_cut=-7000 high_cut=7000 freq=%0.3f", freq / 1000.0);
            break;
        case Modulation::TUNE_REAL:
            currentModulation = mod;
            snprintf(buf, sizeof(buf), "SET mod=usb low_cut=0 high_cut=8000 freq=%0.3f", (freq - 3000) / 1000.0);
            break;
        }
        wsClient.sendString(buf);
    }

    void stop() {
        strcpy(connectionStatus, "Disconnecting..");
        wsClient.stopSocket();
        strcpy(connectionStatus, "Disconnecting2..");
        while (running) {
            kiwiSleepMicros(100000);
        }
        strcpy(connectionStatus, "Disconnected.");
    }

    void start() {
        snprintf(connectionStatus, sizeof connectionStatus, "Connecting..");
        running = true;
        wsClient.stopped = false;
        std::thread looper([this]() {
            kiwiSetThreadName("kiwisdr.wscli");
            bool failed = false;
            try {
                auto ep = kiwiParseSite(hostPort);
                if (!ep.error.empty()) {
                    snprintf(connectionStatus, sizeof connectionStatus, "Error: %s", ep.error.c_str());
                    failed = true;
                }
                else {
                    flog::info("calling x.connectAndReceiveLoop..");
                    wsClient.connectAndReceiveLoop(ep.host, ep.port, "/kiwi/" + std::to_string(kiwiNowMillis()) + "/SND");
                    flog::info("x.connectAndReceiveLoop exited.");
                    snprintf(connectionStatus, sizeof connectionStatus, "Disconnected");
                }
            }
            catch (const std::exception& e) {
                flog::error("KiwiSDRSourceModule: Exception: {}", e.what());
                snprintf(connectionStatus, sizeof connectionStatus, "Error: %s", e.what());
                failed = true;
            }
            catch (...) {
                snprintf(connectionStatus, sizeof connectionStatus, "Error: unknown");
                failed = true;
            }
            if (failed) {
                try { onDisconnected(); } catch (...) {}
            }
            running = false;
        });
        looper.detach();
    }
};
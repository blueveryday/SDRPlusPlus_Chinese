#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <gui/style.h>
#include <utils/net.h>
#include <utils/proto/http.h>
#include <json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include "geomap.h"
#include "kiwi_util.h"
#include "kiwi_widgets.h"
#include "kiwisdr_client.h"

struct KiwiSDRMapSelector {

    struct ServerEntry {
        ImVec2 gps = ImVec2(0, 0); // map coordinates, -1 .. 1
        std::string name;
        std::string loc;
        std::string url;
        std::string antenna;
        float maxSnr = 0;
        float secondSnr = 0;
        int users = 0, usersmax = 0;
        bool antConnected = true;   // false => server report ant_connected="0"（ant disconnected）
        bool selected = false;
    };

    // Everything the worker threads touch lives here, so a thread that outlives the selector never dangles.
    struct State {
        std::mutex mtx; // guards everything below except the atomics
        std::vector<ServerEntry> servers;
        std::string listError;
        std::string testStatus;
        std::string testedServer; // host:port that passed the test
        std::string testedLoc;
        std::atomic<bool> listReady{ false };
        std::atomic<bool> loadingList{ false };
        std::atomic<bool> testInProgress{ false };
    };

    geomap::GeoMap geoMap;
    std::shared_ptr<State> st = std::make_shared<State>();
    std::string root;
    ConfigManager* config;
    const std::string configPrefix;
    const std::string popupName;

    KiwiSDRMapSelector(const std::string& root, ConfigManager* config, const std::string& configPrefix)
        : root(root), config(config), configPrefix(configPrefix), popupName("KiwiSDR 地图选站##kiwisdr_map_" + configPrefix) {
        geoMap.loadFrom(*config, configPrefix.c_str());
    }

    void openPopup() {
        ImGui::OpenPopup(popupName.c_str());
    }

    static std::string toHostPort(const KiwiEndpoint& ep) {
        if (ep.host.find(':') != std::string::npos) { return "[" + ep.host + "]:" + std::to_string(ep.port); }
        return ep.host + ":" + std::to_string(ep.port);
    }

    void drawPopup(std::function<void(const std::string&, const std::string&)> onSelected) {
        ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.125f);
        if (!ImGui::BeginPopupModal(popupName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }
        const ImVec2 ws = ImGui::GetIO().DisplaySize * 0.75f;
        ImGui::SetWindowSize(ws);
        ImGui::BeginChild("##geomap-kiwisdr", ws - ImVec2(0, 50), true, 0);
        geoMap.draw();
        if (geoMap.scaleTranslateDirty) {
            geoMap.saveTo(*config, configPrefix.c_str());
            geoMap.scaleTranslateDirty = false;
        }

        if (!st->listReady) {
            std::string err;
            {
                std::lock_guard<std::mutex> lk(st->mtx);
                err = st->listError;
            }
            if (!err.empty()) {
                ImGui::TextWrapped("%s", err.c_str());
                if (doFingerButton("重试##kiwi-retry")) {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    st->listError.clear();
                }
            }
            else {
                ImGui::Text("正在加载 KiwiSDR 服务器列表...");
                bool expected = false;
                if (st->loadingList.compare_exchange_strong(expected, true)) {
                    auto state = st;
                    auto rootDir = root;
                    std::thread([state, rootDir]() { loadList(state, rootDir); }).detach();
                }
            }
        }
        else {
            std::lock_guard<std::mutex> lk(st->mtx);
            drawServers();
        }

        ImGui::EndChild();

        if (doFingerButton("取消")) {
            ImGui::CloseCurrentPopup();
        }
        std::string tested, testedLoc;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            tested = st->testedServer;
            testedLoc = st->testedLoc;
        }
        if (!tested.empty()) {
            ImGui::SameLine();
            if (doFingerButton("使用已测试的服务器: " + tested)) {
                onSelected(tested, testedLoc);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

private:
    // Called with st->mtx held.
    void drawServers() {
        auto& servers = st->servers;
		int nUsable = 0, nFull = 0, nNoAnt = 0;
		for (const auto& s : servers) {
			bool full = (s.usersmax > 0 && s.users >= s.usersmax);
			if (full)                 { nFull++; }
			else if (!s.antConnected) { nNoAnt++; }
			else                      { nUsable++; }
		}

		ImGui::Text("已加载 %d 个服务器", (int)servers.size());
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),  "| 可用 %d", nUsable);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "| 满员 %d", nFull);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.75f, 1.0f), "| 天线未接 %d", nNoAnt);

        auto sz = style::baseFont->FontSize;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (auto& s : servers) {
            bool full = (s.usersmax > 0 && s.users >= s.usersmax);

            auto dest = geoMap.recentMapToScreen(s.gps);

            // color ：full user > ant disconnected > according to SNR gray/green
            ImColor color;
            if (full) {
                color = ImColor(1.0f, 0.55f, 0.0f);          // orange：Full user
            }
            else if (!s.antConnected) {
                color = ImColor(0.75f, 0.2f, 0.75f);         // Purplish-red：Ant disconnected
            }
            else {
                color = ImColor(0.3f, 0.3f, 0.3f);
                if (s.maxSnr > 22) {
                    color = ImColor(0.0f, 1.0f, 0.0f);
                }
                else if (s.maxSnr > 12) {
                    color = ImColor(0.6f, 0.6f, 0.6f);
                }
            }

            drawList->AddRectFilled(geoMap.recentCanvasPos + dest - ImVec2(sz / 2, sz / 2), geoMap.recentCanvasPos + dest + ImVec2(sz / 2, sz / 2), color, sz / 4.0f);
            if (s.selected) {
                drawList->AddRect(geoMap.recentCanvasPos + dest - ImVec2(sz / 2, sz / 2), geoMap.recentCanvasPos + dest + ImVec2(sz / 2, sz / 2), ImColor(1.0f, 1.0f, 0.0f), sz / 4.0f);
                drawList->AddRect(geoMap.recentCanvasPos + dest - ImVec2(sz / 2 - 1, sz / 2 - 1), geoMap.recentCanvasPos + dest + ImVec2(sz / 2 - 1, sz / 2 - 1), ImColor(1.0f, 1.0f, 0.0f), sz / 4.0f);
            }
            else {
                drawList->AddRect(geoMap.recentCanvasPos + dest - ImVec2(sz / 2, sz / 2), geoMap.recentCanvasPos + dest + ImVec2(sz / 2, sz / 2), ImColor(0.0f, 0.0f, 0.0f), sz / 4.0f);
            }
        }

        // Pick the marker under the cursor (searching a wider radius if nothing is hit)
        if (ImGui::IsMouseClicked(0) && ImGui::GetMouseClickedCount(0) == 1) {
            auto clickPos = ImGui::GetMousePos() - geoMap.recentCanvasPos;
            auto radius = sz / 2;
            for (int q = 0; q < 2; q++) {
                bool found = false;
                for (int i = (int)servers.size() - 1; i >= 0; i--) {
                    auto& it = servers[i];
                    auto dest = geoMap.recentMapToScreen(it.gps);
                    auto dist = sqrt(pow(dest.x - clickPos.x, 2) + pow(dest.y - clickPos.y, 2));
                    if (dist < radius) {
                        found = true;
                        for (auto& s : servers) {
                            s.selected = false;
                        }
                        auto it0 = it;
                        it0.selected = true;
                        servers.emplace_back(it0); // move to the end so it is drawn on top
                        servers.erase(servers.begin() + i);
                        break;
                    }
                }
                if (found) { break; }
                radius *= 5;
            }
        }

        for (auto& s : servers) {
            if (!s.selected) { continue; }
            ImGui::Text("%s", s.name.c_str());
            ImGui::Text("%s", s.loc.c_str());
            if (!s.antenna.empty()) { ImGui::Text("天线: %s", s.antenna.c_str()); }
            if (s.maxSnr > 0) { ImGui::Text("SNR: %d", (int)s.maxSnr); }
            if (s.usersmax > 0) { ImGui::Text("用户: %d/%d", s.users, s.usersmax); }
            bool full = (s.usersmax > 0 && s.users >= s.usersmax);
            if (full) {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "状态: 满员，可能连不上");
            }
            if (!s.antConnected) {
                ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.75f, 1.0f), "状态: 天线未连接，几乎无信号");
            }
            ImGui::Text("URL: %s", s.url.c_str());
            bool busy = st->testInProgress;
            ImGui::BeginDisabled(busy);
            bool doTest = doFingerButton("测试服务器");
            ImGui::EndDisabled();
            if (doTest && !busy) {
                st->testedServer.clear();
                st->testedLoc.clear();
                st->testInProgress = true;
                st->testStatus = "正在测试 " + s.url + " ...";
                auto state = st;
                auto copy = s;
                std::thread([state, copy]() { testServer(state, copy); }).detach();
            }
        }
        if (!st->testStatus.empty()) {
            ImGui::TextWrapped("%s", st->testStatus.c_str());
        }
    }

    static void setStatus(const std::shared_ptr<State>& state, const std::string& text) {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->testStatus = text;
    }

    static void testServer(std::shared_ptr<State> state, ServerEntry s) {
        auto ep = kiwiParseSite(s.url);
        if (!ep.error.empty()) {
            setStatus(state, "无法使用该地址 (" + s.url + "): " + ep.error);
            state->testInProgress = false;
            return;
        }
        std::string hostPort = toHostPort(ep);
        setStatus(state, "正在测试 " + hostPort + " ...");

        KiwiSDRClient testClient;
        std::atomic<bool> connected{ false };
        std::atomic<bool> disconnected{ false };
        bool gotData = false;

        testClient.init(hostPort);
        testClient.onConnected = [&]() {
            connected = true;
            setStatus(state, "已连接 " + hostPort + " ...");
            testClient.tune(14074000, KiwiSDRClient::TUNE_IQ);
        };
        testClient.onDisconnected = [&]() {
            disconnected = true;
        };
        testClient.start();

        auto start = kiwiNowMillis();
        while (!disconnected) {
            testClient.iqDataLock.lock();
            auto bufsize = testClient.iqData.size();
            testClient.iqDataLock.unlock();
            if (bufsize > 0) {
                gotData = true;
                break;
            }
            auto now = kiwiNowMillis();
            if ((connected && now > start + 5000) || now > start + 15000) {
                break;
            }
            kiwiSleepMicros(100000);
        }
        testClient.stop(); // blocks until the client thread is gone, so the locals above stay valid

        std::lock_guard<std::mutex> lk(state->mtx);
        if (gotData) {
            state->testStatus = "已收到数据, 服务器可用: " + hostPort;
            state->testedServer = hostPort;
            state->testedLoc = s.loc;
        }
        else {
            state->testStatus = "未收到数据, 服务器不可用: " + hostPort;
        }
        state->testInProgress = false;
    }

    static std::string dechunk(const std::string& in) {
        std::string out;
        size_t pos = 0;
        while (pos < in.size()) {
            auto eol = in.find("\r\n", pos);
            if (eol == std::string::npos) { break; }
            unsigned long n = strtoul(in.substr(pos, eol - pos).c_str(), nullptr, 16);
            pos = eol + 2;
            if (n == 0) { break; }
            if (pos + n > in.size()) {
                out.append(in, pos, std::string::npos);
                break;
            }
            out.append(in, pos, n);
            pos += n + 2;
        }
        return out;
    }

    // The file is "var kiwisdr_com = [ {...}, {...}, ];" -> take the array and drop the trailing comma.
    static std::string extractListText(const std::string& response) {
        auto begin = response.find("kiwisdr_com");
        begin = (begin == std::string::npos) ? std::string::npos : response.find('[', begin);
        auto end = response.rfind(']');
        if (begin == std::string::npos || end == std::string::npos || end <= begin) {
            throw std::runtime_error("服务器列表格式无效");
        }
        std::string text = response.substr(begin, end - begin + 1);
        auto lastBrace = text.find_last_not_of(" \t\r\n]");
        if (lastBrace != std::string::npos && text[lastBrace] == ',') { text.erase(lastBrace, 1); }
        if (!nlohmann::json::accept(text)) {
            throw std::runtime_error("服务器列表解析失败");
        }
        return text;
    }

    // Returns the JSON array text of the public receiver list (cached for one hour). Throws on error.
    static std::string fetchListText(const std::string& root) {
        std::string cache = root + "/kiwisdr_source.receiverlist.json";
        std::error_code ec;
        if (std::filesystem::exists(cache, ec)) {
            auto mtime = std::filesystem::last_write_time(cache, ec);
            auto age = std::filesystem::file_time_type::clock::now() - mtime;
            if (!ec && age < std::chrono::hours(1)) {
                std::ifstream ifs(cache, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                if (!content.empty() && nlohmann::json::accept(content)) { return content; }
            }
        }

        const std::string host = "rx.linkfanel.net";
        auto sock = net::connect(host, 80);
        net::http::Client http(sock);
        net::http::RequestHeader rq(net::http::METHOD_GET, "/kiwisdr_com.js", host);
        rq.setField("User-Agent", "SDR++/kiwisdr_source");
        rq.setField("Accept", "application/javascript, text/javascript, */*");
        rq.setField("Connection", "close");
        if (http.sendRequestHeader(rq) <= 0) {
            sock->close();
            throw std::runtime_error("发送 HTTP 请求失败");
        }

        net::http::ResponseHeader rs;
        int hdrErr = http.recvResponseHeader(rs, 10000);
        if (hdrErr) {
            sock->close();
            throw std::runtime_error("接收 HTTP 响应头失败 (err=" + std::to_string(hdrErr) + ")");
        }

        int code = (int)rs.getStatusCode();
        if (code < 200 || code >= 300) {
            std::string body;
            std::vector<uint8_t> b(1024);
            int n = sock->recv(b.data(), b.size());
            if (n > 0) { body.append((char*)b.data(), n); }
            sock->close();
            std::string snippet = body.substr(0, 200);
            throw std::runtime_error(
                "服务器返回 HTTP " + std::to_string(code) + " " + rs.getStatusString() +
                " | 响应前200字节: " + snippet);
        }

        std::string response;
        std::vector<uint8_t> buf(65536);
        while (true) {
            int len = sock->recv(buf.data(), buf.size());
            if (len < 1) { break; }
            response.append((char*)buf.data(), len);
        }
        sock->close();

        if (rs.hasField("Transfer-Encoding") &&
            rs.getField("Transfer-Encoding").find("chunked") != std::string::npos) {
            response = dechunk(response);
        }

        std::string text;
        try {
            text = extractListText(response);
        }
        catch (const std::exception& e) {
            throw std::runtime_error(
                std::string(e.what()) + " | 响应前200字节: " + response.substr(0, 200));
        }

        std::ofstream ofs(cache, std::ios::binary);
        if (ofs) { ofs << text; }
        return text;
    }

    static void loadList(std::shared_ptr<State> state, std::string root) {
        try {
            auto list = nlohmann::json::parse(fetchListText(root));
            std::vector<ServerEntry> parsed;
            for (const auto& entry : list) {
                try {
                    if (!entry.is_object() || !entry.contains("gps") || !entry.contains("url") || !entry.contains("offline")) { continue; }
                    if (entry["offline"].get<std::string>() != "no") { continue; }

                    std::stringstream ss(entry["gps"].get<std::string>());
                    ss.imbue(std::locale::classic()); // force '.' as decimal separator
                    geomap::GeoCoordinates geo = { 0.0, 0.0 };
                    char discard;
                    ss >> discard >> geo.latitude >> discard >> geo.longitude >> discard; // "(lat, lon)"
                    if (!ss) { continue; }

                    ServerEntry e;
                    e.gps = geomap::geoToCartesian(geo).toImVec2();
                    e.name = entry.value("name", std::string());
                    e.loc = entry.value("loc", std::string());
                    e.url = entry["url"].get<std::string>();
                    e.antenna = entry.value("antenna", std::string());
                    sscanf(entry.value("snr", std::string()).c_str(), "%f,%f", &e.maxSnr, &e.secondSnr);
                    e.users = atoi(entry.value("users", std::string("0")).c_str());
                    e.usersmax = atoi(entry.value("users_max", std::string("0")).c_str());
                    e.antConnected = entry.value("ant_connected", std::string("1")) != "0";   // default connected
                    parsed.push_back(e);
                }
                catch (const std::exception&) {
                    // skip malformed entry
                }
            }
            std::sort(parsed.begin(), parsed.end(), [](const ServerEntry& a, const ServerEntry& b) { return a.maxSnr < b.maxSnr; });
            flog::info("KiwiSDR map: parsed {} servers", (int)parsed.size());

            std::lock_guard<std::mutex> lk(state->mtx);
            state->servers = std::move(parsed);
            state->listReady = true;
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->listError = std::string("加载服务器列表失败: ") + e.what();
        }
        state->loadingList = false;
    }
};
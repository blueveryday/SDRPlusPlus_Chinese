#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <json.hpp>
#include <core.h>
#include <utils/flog.h>

// 网络信号源（SpyServer、SDR++ Server、RTL-TCP、RFspace、Spectran HTTP、网络源）的“地址历史”。
//
// 每个信号源各自保存一份“地址 + 端口”的历史（最近用过的在最前面，最多 kMaxEntries 条），
// 放在配置目录的 host_history.json 里。界面上在地址栏上方放一个下拉框，选一条就同时填好地址和端口。
//
// 用法（以 SmGui 为例；用普通 ImGui 的模块把 SmGui::Combo 换成 ImGui::Combo 即可）：
//   成员：  HostHistory hostHistory;  int hostHistoryId = 0;
//   构造：  hostHistory.load("spyserver");
//   界面：  SmGui::FillWidth();
//           if (SmGui::Combo("##hist", &hostHistoryId, hostHistory.items())) {
//               if (hostHistory.select(hostHistoryId, hostname, sizeof(hostname), &port) == HostHistory::SELECTED) { /* 保存配置 */ }
//               hostHistoryId = 0;
//           }
//   连接成功后：  hostHistory.add(hostname, port);
class HostHistory {
public:
    static const size_t kMaxEntries = 20;

    struct Entry {
        std::string host;
        int port = 0;
    };

    // select() 的返回值
    enum SelectResult {
        NOTHING = 0,  // 选的是占位项，什么都没发生
        SELECTED = 1, // 已把地址和端口填进去
        CLEARED = 2   // 清空了历史
    };

    // 读取某个信号源的历史。key 是信号源的名字，例如 "spyserver"。
    void load(const std::string& key) {
        this->key = key;
        entries.clear();

        nlohmann::json doc = readFile();
        if (doc.is_object() && doc.contains(key) && doc[key].is_array()) {
            for (auto& e : doc[key]) {
                if (!e.is_object()) { continue; }
                if (!e.contains("host") || !e["host"].is_string()) { continue; }
                if (!e.contains("port") || !e["port"].is_number_integer()) { continue; }
                Entry en;
                en.host = e["host"].get<std::string>();
                en.port = e["port"].get<int>();
                if (en.host.empty() || en.port < 1 || en.port > 65535) { continue; }
                entries.push_back(en);
                if (entries.size() >= kMaxEntries) { break; }
            }
        }
        rebuild();
    }

    // 记住一条地址。已经有的会移到最前面。地址为空或端口不合法时忽略。
    void add(const std::string& hostIn, int port) {
        std::string host = trim(hostIn);
        if (host.empty() || port < 1 || port > 65535) { return; }

        // 已经在最前面就不用动（避免每次连接都写文件）
        if (!entries.empty() && same(entries.front(), host, port)) { return; }

        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return same(e, host, port); }), entries.end());
        Entry en;
        en.host = host;
        en.port = port;
        entries.insert(entries.begin(), en);
        if (entries.size() > kMaxEntries) { entries.resize(kMaxEntries); }

        rebuild();
        save();
    }

    // 清空历史
    void clear() {
        if (entries.empty()) { return; }
        entries.clear();
        rebuild();
        save();
    }

    // 下拉框的选项（用 \0 分隔，末尾两个 \0）：第 0 项是占位提示，后面是每条历史，最后一项是“清除历史记录”。
    const char* items() const {
        return itemsTxt.c_str();
    }

    size_t count() const {
        return entries.size();
    }

    // 处理下拉框的选择。index 是下拉框返回的下标。
    // 选中某条历史时，把地址写进 hostBuf（最多 hostBufSize-1 个字节）、端口写进 *port，返回 SELECTED。
    SelectResult select(int index, char* hostBuf, size_t hostBufSize, int* port) {
        if (index <= 0 || entries.empty()) { return NOTHING; }
        if ((size_t)index <= entries.size()) {
            const Entry& e = entries[index - 1];
            if (hostBuf && hostBufSize > 0) {
                snprintf(hostBuf, hostBufSize, "%s", e.host.c_str());
            }
            if (port) { *port = e.port; }
            return SELECTED;
        }
        if ((size_t)index == entries.size() + 1) {
            clear();
            return CLEARED;
        }
        return NOTHING;
    }

private:
    static std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char)s[b])) { b++; }
        while (e > b && std::isspace((unsigned char)s[e - 1])) { e--; }
        return s.substr(b, e - b);
    }

    // 域名不区分大小写
    static bool same(const Entry& e, const std::string& host, int port) {
        if (e.port != port || e.host.size() != host.size()) { return false; }
        for (size_t i = 0; i < host.size(); i++) {
            if (std::tolower((unsigned char)e.host[i]) != std::tolower((unsigned char)host[i])) { return false; }
        }
        return true;
    }

    // 下拉框里显示的文字。IPv6 地址要加方括号，不然看不出端口
    static std::string label(const Entry& e) {
        if (e.host.find(':') != std::string::npos) { return "[" + e.host + "]:" + std::to_string(e.port); }
        return e.host + ":" + std::to_string(e.port);
    }

    void rebuild() {
        itemsTxt.clear();
        if (entries.empty()) {
            itemsTxt += "历史记录（空）";
            itemsTxt += '\0';
            return;
        }
        itemsTxt += "历史记录...";
        itemsTxt += '\0';
        for (const Entry& e : entries) {
            itemsTxt += label(e);
            itemsTxt += '\0';
        }
        itemsTxt += "清除历史记录";
        itemsTxt += '\0';
    }

    static std::string filePath() {
        return core::args["root"].s() + "/host_history.json";
    }

    static nlohmann::json readFile() {
        try {
            std::ifstream f(filePath());
            if (!f.is_open()) { return nlohmann::json::object(); }
            nlohmann::json doc = nlohmann::json::parse(f);
            return doc.is_object() ? doc : nlohmann::json::object();
        }
        catch (const std::exception& e) {
            flog::warn("Could not read host history: {}", e.what());
            return nlohmann::json::object();
        }
    }

    // 读出整个文件、只改自己这一项再写回，不影响其他信号源的历史
    void save() {
        try {
            nlohmann::json doc = readFile();
            nlohmann::json arr = nlohmann::json::array();
            for (const Entry& e : entries) {
                arr.push_back({ { "host", e.host }, { "port", e.port } });
            }
            doc[key] = arr;

            std::ofstream f(filePath());
            if (!f.is_open()) { return; }
            f << doc.dump(1);
        }
        catch (const std::exception& e) {
            flog::warn("Could not save host history: {}", e.what());
        }
    }

    std::string key;
    std::vector<Entry> entries;
    std::string itemsTxt = std::string("历史记录（空）") + '\0';
};

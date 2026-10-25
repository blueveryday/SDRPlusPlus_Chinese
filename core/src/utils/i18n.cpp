#include "i18n.h"
#include <json.hpp>
#include <atomic>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cstring>

using json = nlohmann::json;

namespace i18n {
    namespace {
        // 一份生效的翻译表。创建后不再修改。
        struct Table {
            std::string code;
            std::unordered_map<std::string, std::string> map;                  // 原文 -> 译文
            std::unordered_map<std::string_view, const std::string*> view;     // 用于不分配内存的查找，指向 map 里的键和值
            size_t minLen = (size_t)-1;
            size_t maxLen = 0;
        };

        std::atomic<const Table*> g_current{ nullptr };
        std::atomic<bool> g_active{ false };
        std::mutex g_mtx;                                  // 保护 g_owned / g_retired
        std::shared_ptr<Table> g_owned;
        // 被替换下来的旧表不立即释放：界面线程可能刚拿到指向旧表的指针。数量很少，保留最近几份即可。
        std::deque<std::shared_ptr<Table>> g_retired;

        const size_t kMaxFileSize = 16 * 1024 * 1024;
        const size_t kMaxWarnings = 50;

        void addWarning(LangInfo& info, const std::string& msg) {
            info.skipped++;
            if (info.warnings.size() < kMaxWarnings) { info.warnings.push_back(msg); }
        }

        std::string shorten(const std::string& s, size_t maxBytes = 40) {
            if (s.size() <= maxBytes) { return s; }
            size_t cut = maxBytes;
            // 不要把 UTF-8 多字节字符截断一半
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) { cut--; }
            return s.substr(0, cut) + "...";
        }

        // 把字节偏移换算成“第几行第几列”，方便提示用户语言文件哪里写错了
        std::string lineColumn(const std::string& text, size_t offset) {
            size_t line = 1, col = 1;
            size_t end = std::min(offset, text.size());
            for (size_t i = 0; i < end; i++) {
                if (text[i] == '\n') { line++; col = 1; }
                else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) { col++; }
            }
            return "第 " + std::to_string(line) + " 行，第 " + std::to_string(col) + " 列";
        }

        bool readFile(const std::string& path, std::string& out, std::string& err) {
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open()) { err = "无法打开文件"; return false; }
            f.seekg(0, std::ios::end);
            std::streamoff size = f.tellg();
            f.seekg(0, std::ios::beg);
            if (size < 0 || (size_t)size > kMaxFileSize) { err = "文件过大（超过 16 MB）"; return false; }
            out.resize((size_t)size);
            if (size > 0) { f.read(&out[0], size); }
            if (!f) { err = "读取文件失败"; return false; }
            // 去掉 UTF-8 BOM（Windows 记事本保存时会带上）
            if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
                out.erase(0, 3);
            }
            return true;
        }

        std::string fileStem(const std::string& path) {
            size_t slash = path.find_last_of("/\\");
            std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            size_t dot = name.find_last_of('.');
            return (dot == std::string::npos) ? name : name.substr(0, dot);
        }

        std::string getString(const json& obj, const char* key) {
            if (!obj.is_object()) { return ""; }
            auto it = obj.find(key);
            if (it == obj.end() || !it->is_string()) { return ""; }
            return it->get<std::string>();
        }

        // 把 strings 里的内容摊平成“原文 -> 译文”。允许用嵌套对象给条目分组：
        //   "strings": { "扫描器": { "开始": "Start" }, "导入": "Import" }
        // 以 "//" 或 "_" 开头的键是注释，会被忽略。
        void flatten(const json& node, std::vector<std::pair<std::string, std::string>>& out, LangInfo& info) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string& key = it.key();
                if (key.size() >= 2 && key[0] == '/' && key[1] == '/') { continue; }
                if (!key.empty() && key[0] == '_') { continue; }
                const json& val = it.value();
                if (val.is_object()) { flatten(val, out, info); continue; }
                if (!val.is_string()) {
                    addWarning(info, "「" + shorten(key) + "」的译文不是字符串，已忽略");
                    continue;
                }
                out.emplace_back(key, val.get<std::string>());
            }
        }

        // 解析语言文件。table 可以为 nullptr（只检查不生成表）。
        bool parseFile(const std::string& path, LangInfo& info, std::shared_ptr<Table>* table) {
            info = LangInfo();
            info.path = path;

            std::string text, err;
            if (!readFile(path, text, err)) { info.error = err; return false; }

            json doc;
            try {
                doc = json::parse(text);
            }
            catch (const json::parse_error& e) {
                info.error = "JSON 格式错误（" + lineColumn(text, e.byte > 0 ? e.byte - 1 : 0) + "）：" + e.what();
                return false;
            }
            catch (const std::exception& e) {
                info.error = std::string("JSON 解析失败：") + e.what();
                return false;
            }

            if (!doc.is_object()) { info.error = "文件最外层必须是一个 JSON 对象 { ... }"; return false; }

            const json& meta = doc.contains("meta") ? doc["meta"] : json();
            info.code = getString(meta, "code");
            if (info.code.empty()) { info.code = fileStem(path); }
            info.name = getString(meta, "name");
            info.nativeName = getString(meta, "native_name");
            if (info.nativeName.empty()) { info.nativeName = info.name.empty() ? info.code : info.name; }
            if (info.name.empty()) { info.name = info.nativeName; }
            info.author = getString(meta, "author");
            info.version = getString(meta, "version");

            if (!doc.contains("strings") || !doc["strings"].is_object()) {
                info.error = "缺少 \"strings\" 对象（原文 -> 译文 对照表）";
                return false;
            }

            std::vector<std::pair<std::string, std::string>> items;
            flatten(doc["strings"], items, info);

            std::shared_ptr<Table> t;
            if (table) { t = std::make_shared<Table>(); t->code = info.code; }

            std::unordered_map<std::string, std::string> accepted;
            for (auto& [key, val] : items) {
                if (key.empty()) { addWarning(info, "存在空的原文，已忽略"); continue; }
                if (val.empty() || val == key) { continue; } // 没有翻译，保持原文，不占用查找表
                if (formatSignature(key) != formatSignature(val)) {
                    addWarning(info, "「" + shorten(key) + "」的译文中 % 格式符（如 %d、%s、%.1f）与原文不一致，已忽略");
                    continue;
                }
                accepted[key] = val; // 重复的原文以后出现的为准
            }
            info.entries = (int)accepted.size();

            if (t) {
                t->map = std::move(accepted);
                for (auto& [k, v] : t->map) {
                    t->view[std::string_view(k)] = &v;
                    t->minLen = std::min(t->minLen, k.size());
                    t->maxLen = std::max(t->maxLen, k.size());
                }
                *table = t;
            }
            return true;
        }

        void install(std::shared_ptr<Table> t) {
            std::lock_guard<std::mutex> lck(g_mtx);
            if (g_owned) {
                g_retired.push_back(g_owned);
                while (g_retired.size() > 8) { g_retired.pop_front(); }
            }
            g_owned = t;
            const bool active = (t && !t->map.empty());
            g_current.store(active ? t.get() : nullptr);
            g_active.store(active);
        }
    }

    std::string formatSignature(const std::string& s) {
        std::string sig;
        size_t i = 0, n = s.size();
        while (i < n) {
            if (s[i] != '%') { i++; continue; }
            size_t j = i + 1;
            if (j < n && s[j] == '%') { i = j + 1; continue; } // "%%" 是字面的百分号

            std::string spec;
            while (j < n && std::strchr("-+ #0'", s[j])) { j++; }                         // 标志
            if (j < n && s[j] == '*') { spec += '*'; j++; }                                // 宽度由参数给出
            else { while (j < n && s[j] >= '0' && s[j] <= '9') { j++; } }
            if (j < n && s[j] == '.') {                                                    // 精度
                j++;
                if (j < n && s[j] == '*') { spec += '*'; j++; }
                else { while (j < n && s[j] >= '0' && s[j] <= '9') { j++; } }
            }
            while (j < n && std::strchr("hlLqjzt", s[j])) { spec += s[j]; j++; }           // 长度修饰
            if (j < n && std::strchr("diouxXeEfFgGaAcspn", s[j])) {
                spec += s[j];
                if (!sig.empty()) { sig += '|'; }
                sig += spec;
                i = j + 1;
            }
            else {
                i = i + 1; // 不是合法的格式符（例如 "100%" 或 "50% 增益"），当作普通字符
            }
        }
        return sig;
    }

    LangInfo inspect(const std::string& path) {
        LangInfo info;
        parseFile(path, info, nullptr);
        return info;
    }

    bool load(const std::string& path, LangInfo* infoOut) {
        LangInfo info;
        std::shared_ptr<Table> t;
        bool ok = parseFile(path, info, &t);
        if (infoOut) { *infoOut = info; }
        if (!ok) { return false; }
        install(t);
        return true;
    }

    void reset() {
        install(nullptr);
    }

    std::string currentCode() {
        std::lock_guard<std::mutex> lck(g_mtx);
        return g_owned ? g_owned->code : "";
    }

    int entryCount() {
        const Table* t = g_current.load();
        return t ? (int)t->map.size() : 0;
    }

    const char* tr(const char* text) {
        if (!text || !g_active.load(std::memory_order_relaxed)) { return text; }
        size_t outLen = 0;
        const char* r = lookup(text, std::strlen(text), &outLen);
        return r ? r : text;
    }

    bool isActive() {
        return g_active.load(std::memory_order_relaxed);
    }

    const char* lookup(const char* text, size_t len, size_t* outLen) {
        if (!g_active.load(std::memory_order_relaxed)) { return nullptr; }
        const Table* t = g_current.load(std::memory_order_acquire);
        if (!t || len < t->minLen || len > t->maxLen) { return nullptr; }
        auto it = t->view.find(std::string_view(text, len));
        if (it == t->view.end()) { return nullptr; }
        *outLen = it->second->size();
        return it->second->c_str();
    }
}

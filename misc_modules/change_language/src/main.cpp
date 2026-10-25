#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <config.h>
#include <utils/flog.h>
#include <utils/i18n.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <vector>
#ifdef __ANDROID__
#include <backend.h>
#else
#include <gui/file_dialogs.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "change_language",
    /* Description:     */ "界面语言切换 module for SDR++",
    /* Author:          */ "SDR++ CE",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

namespace fs = std::filesystem;

class ChangeLanguageModule : public ModuleManager::Instance {
public:
    ChangeLanguageModule(std::string name) {
        this->name = name;
        root = (std::string)core::args["root"];

        // 内置语言文件目录：<资源目录>/lang；用户自己的语言文件目录：<root>/lang（升级程序时不会被覆盖）
        core::configManager.acquire();
        std::string resDir = core::configManager.conf["resourcesDirectory"];
        core::configManager.release();
        builtinDir = (fs::absolute(resDir) / "lang").string();
        userDir = (fs::absolute(root) / "lang").string();
        std::error_code ec;
        fs::create_directories(userDir, ec);

        scan();

        config.acquire();
        std::string wanted = config.conf.value("language", std::string("en-US"));
        config.release();
        applyCode(wanted, false);

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~ChangeLanguageModule() {
        gui::menu.removeEntry(name);
#ifndef __ANDROID__
        if (dialogOpen && dialog) { delete dialog; dialog = nullptr; dialogOpen = false; }
#endif
        // 模块被删除后，界面恢复为原文（中文）
        i18n::reset();
    }

    void postInit() {}

    void enable() {
        enabled = true;
        applyCode(selectedCode, false);
    }

    void disable() {
        enabled = false;
        i18n::reset();
    }

    bool isEnabled() {
        return enabled;
    }

private:
    struct Entry {
        i18n::LangInfo info;
        bool fromUser = false;
    };

    // ---- 语言文件列表 ----
    void scanDir(const std::string& dir, bool fromUser, std::map<std::string, Entry>& found) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) { return; }
        for (auto& item : fs::directory_iterator(dir, ec)) {
            if (!item.is_regular_file(ec)) { continue; }
            std::string ext = item.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".json") { continue; }

            Entry e;
            e.info = i18n::inspect(item.path().string());
            e.fromUser = fromUser;
            if (!e.info.error.empty()) {
                broken.push_back({ item.path().string(), e.info.error });
                continue;
            }
            found[e.info.code] = e; // 用户目录后扫描，同一语言标签时覆盖内置的
        }
    }

    void scan() {
        broken.clear();
        std::map<std::string, Entry> found;
        scanDir(builtinDir, false, found);
        scanDir(userDir, true, found);

        entries.clear();
        // zh-CN 是源码原文，即使语言文件缺失也要能选
        if (found.find("zh-CN") == found.end()) {
            Entry e;
            e.info.code = "zh-CN";
            e.info.name = "Chinese (Simplified)";
            e.info.nativeName = "简体中文";
            found["zh-CN"] = e;
        }
        for (auto& [code, e] : found) { entries.push_back(e); }
        // zh-CN 排第一，其余按语言标签排序（map 已排序）
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return (a.info.code == "zh-CN") && (b.info.code != "zh-CN");
        });

        itemsTxt.clear();
        for (auto& e : entries) {
            itemsTxt += e.info.nativeName + " (" + e.info.code + ")";
            itemsTxt += '\0';
        }
        selectedIdx = 0;
        for (int i = 0; i < (int)entries.size(); i++) {
            if (entries[i].info.code == selectedCode) { selectedIdx = i; }
        }
    }

    // ---- 应用语言 ----
    void applyCode(const std::string& code, bool save) {
        status.clear();
        warnings.clear();
        statusIsError = false;

        const Entry* target = nullptr;
        for (auto& e : entries) {
            if (e.info.code == code) { target = &e; break; }
        }
        if (!target) {
            // 配置里记的语言文件已经不存在了：回到原文
            i18n::reset();
            selectedCode = "zh-CN";
            selectedIdx = 0;
            if (code != "zh-CN") {
                status = "找不到语言文件，已恢复为简体中文";
                statusIsError = true;
            }
            return;
        }

        if (target->info.path.empty()) {
            i18n::reset(); // 没有 zh-CN.json 时直接使用源码原文
            loadedInfo = target->info;
        }
        else {
            i18n::LangInfo info;
            if (!i18n::load(target->info.path, &info)) {
                status = "加载失败：" + info.error;
                statusIsError = true;
                flog::error("加载语言文件失败 {0}: {1}", target->info.path, info.error);
                return;
            }
            loadedInfo = info;
            warnings = info.warnings;
        }

        selectedCode = target->info.code;
        for (int i = 0; i < (int)entries.size(); i++) {
            if (entries[i].info.code == selectedCode) { selectedIdx = i; }
        }
        if (save) {
            config.acquire();
            config.conf["language"] = selectedCode;
            config.release(true);
        }
        flog::info("界面语言: {0} ({1} 条翻译)", selectedCode, loadedInfo.entries);
    }

    // ---- 加载用户选择的语言文件 ----
    static std::string safeCode(const std::string& code) {
        std::string out;
        for (char c : code) {
            if (std::isalnum((unsigned char)c) || c == '-' || c == '_') { out += c; }
        }
        return out;
    }

    void importFile(const std::string& path) {
        i18n::LangInfo info = i18n::inspect(path);
        if (!info.error.empty()) {
            status = "无法使用该文件：" + info.error;
            statusIsError = true;
            warnings.clear();
            return;
        }

        // 复制到用户语言目录，之后可以直接在列表里选择，下次启动也还在
        std::string code = safeCode(info.code);
        if (code.empty()) { code = "custom"; }
        std::string dest = (fs::path(userDir) / (code + ".json")).string();
        std::error_code ec;
        bool same = fs::exists(dest, ec) && fs::equivalent(dest, path, ec);
        if (!same) {
            fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                // 复制不成功也允许本次直接使用
                i18n::LangInfo li;
                if (i18n::load(path, &li)) {
                    loadedInfo = li;
                    warnings = li.warnings;
                    selectedCode = li.code;
                    status = "已加载（无法复制到语言目录，仅本次有效）";
                    statusIsError = false;
                }
                return;
            }
        }
        scan();
        applyCode(info.code, true);
        if (!statusIsError) { status = "已加载语言文件，并保存到语言目录"; }
    }

    void openFileDialog() {
#ifdef __ANDROID__
        backend::openFileDialog([this](std::string path) {
            if (!path.empty()) { importFile(path); }
        });
#else
        if (dialogOpen) { return; }
        dialogOpen = true;
        dialog = new pfd::open_file("选择语言文件", "", { "语言文件 (*.json)", "*.json", "所有文件", "*" });
#endif
    }

    // ---- 界面 ----
    static void menuHandler(void* ctx) {
        ChangeLanguageModule* _this = (ChangeLanguageModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

#ifndef __ANDROID__
        if (_this->dialogOpen && _this->dialog && _this->dialog->ready()) {
            std::vector<std::string> paths = _this->dialog->result();
            delete _this->dialog;
            _this->dialog = nullptr;
            _this->dialogOpen = false;
            if (!paths.empty()) { _this->importFile(paths[0]); }
        }
#endif

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("界面语言");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_change_lang_sel_", _this->name), &_this->selectedIdx, _this->itemsTxt.c_str())) {
            if (_this->selectedIdx >= 0 && _this->selectedIdx < (int)_this->entries.size()) {
                _this->applyCode(_this->entries[_this->selectedIdx].info.code, true);
            }
        }

        float half = (menuWidth - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        if (ImGui::Button(CONCAT("加载语言文件...##_change_lang_load_", _this->name), ImVec2(half, 0))) {
            _this->openFileDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("重新加载##_change_lang_reload_", _this->name), ImVec2(half, 0))) {
            // 修改语言文件之后点这里即可生效
            std::string code = _this->selectedCode;
            _this->scan();
            _this->applyCode(code, false);
            if (!_this->statusIsError && _this->status.empty()) { _this->status = "已重新加载"; }
        }

        if (!_this->enabled) { style::endDisabled(); }

        // 状态
        if (!_this->status.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            if (_this->statusIsError) { ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _this->status.c_str()); }
            else { ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", _this->status.c_str()); }
            ImGui::PopTextWrapPos();
        }
        ImGui::Text("有效翻译条目: %d", i18n::entryCount());

        if (!_this->warnings.empty() && ImGui::CollapsingHeader("被忽略的翻译条目")) {
            ImGui::PushTextWrapPos(0.0f);
            for (auto& w : _this->warnings) { ImGui::TextUnformatted(w.c_str()); ImGui::Spacing(); }
            ImGui::PopTextWrapPos();
        }

        if (!_this->broken.empty() && ImGui::CollapsingHeader("无法使用的语言文件")) {
            ImGui::PushTextWrapPos(0.0f);
            for (auto& [path, err] : _this->broken) {
                ImGui::TextUnformatted(path.c_str());
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", err.c_str());
                ImGui::Spacing();
            }
            ImGui::PopTextWrapPos();
        }

        if (ImGui::CollapsingHeader("语言文件目录")) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("内置语言文件: %s", _this->builtinDir.c_str());
            ImGui::Text("用户语言文件: %s", _this->userDir.c_str());
            ImGui::TextUnformatted("把新的语言文件（例如 ja-JP.json）放进用户语言文件目录，点“重新加载”即可在列表里选择。");
            ImGui::PopTextWrapPos();
        }
    }

    std::string name;
    bool enabled = true;
    std::string root, builtinDir, userDir;

    std::vector<Entry> entries;
    std::vector<std::pair<std::string, std::string>> broken;
    std::string itemsTxt;
    int selectedIdx = 0;
    std::string selectedCode = "zh-CN";

    i18n::LangInfo loadedInfo;
    std::vector<std::string> warnings;
    std::string status;
    bool statusIsError = false;

#ifndef __ANDROID__
    pfd::open_file* dialog = nullptr;
    bool dialogOpen = false;
#endif
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["language"] = "en-US";
    config.setPath((std::string)core::args["root"] + "/change_language_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ChangeLanguageModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (ChangeLanguageModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}

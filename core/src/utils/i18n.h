#pragma once
#include <string>
#include <vector>
#include <cstddef>

// 界面多语言支持。
//
// 程序源码里的界面文字都是中文（原文）。切换语言时，界面绘制层（ImGui 的文字绘制函数）
// 会按“原文 -> 译文”的对照表替换要显示的文字；找不到对应条目的文字保持原文显示，
// 所以语言文件不需要翻译全部条目。
//
// 语言文件格式见 root/res/lang/README.md。
namespace i18n {
    struct LangInfo {
        std::string code;       // 语言标签，例如 "en-US"、"zh-CN"、"ja-JP"
        std::string name;       // 通用名称（英文），例如 "English (United States)"
        std::string nativeName; // 本语言自称，例如 "English"、"简体中文"
        std::string author;
        std::string version;
        std::string path;       // 语言文件路径
        std::string error;      // 非空表示文件无法使用（读取或解析失败）
        int entries = 0;        // 有效的翻译条目数
        int skipped = 0;        // 因格式问题被忽略的条目数
        std::vector<std::string> warnings; // 被忽略条目的原因（最多保留 50 条）
    };

    // 读取并校验语言文件，不改变当前界面语言。用于列出可用的语言文件。
    LangInfo inspect(const std::string& path);

    // 读取语言文件并立即应用。失败时界面语言保持不变，错误原因在 info->error 中。
    bool load(const std::string& path, LangInfo* info = nullptr);

    // 恢复为源码里的原文（中文）。
    void reset();

    // 当前语言标签；"" 表示原文（没有加载任何语言文件）。
    std::string currentCode();

    // 当前生效的翻译条目数。
    int entryCount();

    // 翻译一条文字：有译文返回译文，否则原样返回。
    // 适合在模块里自己拼接界面文字时使用；返回的指针在下次切换语言前有效。
    const char* tr(const char* text);

    // 计算 printf 风格格式串的“参数签名”，例如 "%d 个 %.1f%%" -> "d|f"。
    // 译文的签名必须和原文一致，否则会导致 printf 参数错位而崩溃，这样的条目会被忽略。
    std::string formatSignature(const std::string& text);

    // ---- 以下由 ImGui 绘制层调用，不需要在其他地方使用 ----
    bool isActive();
    // 查找原文 text[0..len) 的译文；没有译文返回 nullptr，否则返回译文并通过 outLen 给出长度。
    const char* lookup(const char* text, size_t len, size_t* outLen);
}

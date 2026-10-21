#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <functional>
#include <string>
#include <vector>
#include <cstring>

inline void splitString(const std::string& str, const char* sep, const std::function<void(const std::string&)>& callback) {
    const char* c = str.data();
    size_t limit = str.length();
    if (limit == 0) {
        return;
    }
    long long start = 0;
    for (long long i = 0; i < (long long)limit; i++) {
        if (i == (long long)limit - 1) {
            if (strchr(sep, c[i]) != nullptr) {
                callback(std::string(c + start, i - start));
            }
            else {
                callback(std::string(c + start, i - start + 1));
            }
            break;
        }
        if (strchr(sep, c[i]) != nullptr) {
            callback(std::string(c + start, i - start));
            start = i + 1;
        }
    }
}

inline void splitStringV(const std::string& str, const char* sep, std::vector<std::string>& dest) {
    dest.clear();
    splitString(str, sep, [&](const std::string& s) {
        dest.emplace_back(s);
    });
}

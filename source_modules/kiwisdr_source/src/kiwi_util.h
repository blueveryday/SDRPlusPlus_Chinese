#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <chrono>
#include <string>
#include <thread>

#ifdef __linux__
#include <sys/prctl.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

inline long long kiwiNowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline void kiwiSleepMicros(long long micros) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

inline void kiwiSetThreadName(const std::string& name) {
#ifdef __linux__
    prctl(PR_SET_NAME, name.c_str(), 0, 0, 0);
#endif
#ifdef __APPLE__
    pthread_setname_np(name.c_str());
#endif
    (void)name;
}

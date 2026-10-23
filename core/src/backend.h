#pragma once
#include <string>
#include <functional>

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    int renderLoop();
    int end();

    std::string getAppFilesDir();

#ifdef __ANDROID__
    void openFileDialog(std::function<void(std::string)> callback);
    void saveFileDialog(std::string cacheFilePath, std::string defaultName, std::function<void(std::string)> callback);
#endif
}
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <json.hpp>
#include <imgui.h>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <stdint.h>
#include <config.h>

using nlohmann::json;

namespace geomap {

    constexpr double pi = 3.14159265358979323846;
    constexpr double earthRadius = 6371.0; // Earth radius in kilometers

    inline double degToRad(double deg) {
        return deg * pi / 180.0;
    }

    struct GeoCoordinates {
        double latitude;
        double longitude;
    };

    struct CartesianCoordinates {
        double x;
        double y;

        ImVec2 toImVec2() {
            return ImVec2(x, y);
        }
    };


    inline CartesianCoordinates geoToCartesian(const GeoCoordinates& geo) {
        double latRad = degToRad(geo.latitude);
        double lngRad = degToRad(geo.longitude);

        double x = lngRad / pi;
        double y = latRad / (pi / 2.0);

        return { x, y };
    }


    struct GeoMap {

        ImVec2 scale = ImVec2(1.0, 1.0);
        ImVec2 translate = ImVec2(0.0, 0.0); // in map coordinates
        bool scaleTranslateDirty = false;

        std::shared_ptr<ImVec2> initialTouchPos;
        ImVec2 initialTranslate;

        std::function <ImVec2(ImVec2)> recentMapToScreen;
        ImVec2 recentCanvasPos;

        void draw();
        void saveTo(ConfigManager &manager, const char* string);
        void loadFrom(ConfigManager& manager, const char* prefix);
    };

};
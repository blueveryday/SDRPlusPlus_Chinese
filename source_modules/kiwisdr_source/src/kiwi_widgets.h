#pragma once

#include <imgui.h>
#include <string>

// Brown's "finger friendly" button; here it is just a regular button with a bit more padding.
inline bool doFingerButton(const std::string& label) {
    return ImGui::Button(label.c_str());
}

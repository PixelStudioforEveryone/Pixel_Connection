#include "session_wire.h"

namespace pxc::gui {
namespace {

constexpr int kWireVersion = 1;

bool valid_envelope(const nlohmann::json& j) {
    return j.is_object() && j.value("pxc", 0) == kWireVersion &&
           j.contains("kind");
}

}  // namespace

bool parse_session_message(const std::string& text, nlohmann::json& out) {
    if (text.empty() || text.front() != '{') return false;
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (!valid_envelope(j)) return false;
        out = std::move(j);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string encode_session_message(const nlohmann::json& j) {
    nlohmann::json with_tag = j;
    with_tag["pxc"] = kWireVersion;
    return with_tag.dump();
}

nlohmann::json make_video_cfg_cmd(int width, int height, int fps, int bitrate_kbps) {
    nlohmann::json j;
    j["kind"]        = "cmd";
    j["cmd"]         = "video_cfg";
    j["width"]       = width;
    j["height"]      = height;
    j["fps"]         = fps;
    j["bitrate_kbps"] = bitrate_kbps;
    return j;
}

nlohmann::json make_screen_list_cmd() {
    nlohmann::json j;
    j["kind"] = "cmd";
    j["cmd"]  = "screen_list";
    return j;
}

nlohmann::json make_screen_switch_cmd(int index) {
    nlohmann::json j;
    j["kind"]  = "cmd";
    j["cmd"]   = "screen_switch";
    j["index"] = index;
    return j;
}

nlohmann::json make_keyframe_cmd() {
    nlohmann::json j;
    j["kind"] = "cmd";
    j["cmd"]  = "keyframe";
    return j;
}

nlohmann::json make_input_event(const nlohmann::json& payload) {
    nlohmann::json j = payload;
    j["kind"] = "input";
    return j;
}

}  // namespace pxc::gui

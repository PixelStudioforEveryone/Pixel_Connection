#include "pxc/protocol.h"

namespace pxc {

std::string encode(const Message& m) {
    nlohmann::json j = m;
    return j.dump();
}

bool decode(const std::string& text, Message& out) {
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) return false;
        out = j.get<Message>();
        return !out.type.empty();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace pxc

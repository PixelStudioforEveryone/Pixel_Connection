// 可选的纯服务器部署入口，Qt 发布包不生成此程序。
#include "server_runtime.h"
#if defined(_WIN32)
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> values;
    values.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 2;
        std::string value(static_cast<size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                 value.data(), size, nullptr, nullptr)) return 2;
        value.pop_back();
        values.push_back(std::move(value));
    }
    std::vector<char*> args;
    for (auto& value : values) args.push_back(value.data());
    return pxc::server::run_server(argc, args.data());
}
#else
int main(int argc, char** argv) {
    return pxc::server::run_server(argc, argv);
}
#endif

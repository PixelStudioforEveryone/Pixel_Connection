#pragma once

namespace pxc::server {
// 仅在服务器进程调用；GUI 与服务通过同一个程序的两种入口隔离。
int run_server(int argc, char** argv);
}

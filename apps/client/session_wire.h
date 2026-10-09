#pragma once

// ch-control 上的应用层 JSON 线格式（认证之外的一切控制面数据）。
//
// ch-control 是可靠有序通道，文本消息。为与既有认证消息
// （"PXC_AUTH_*" 前缀）区分，JSON 消息一律以 '{' 开头并带固定标记：
//
//   {"pxc":1,"kind":"cmd","cmd":"video_cfg","width":1920,"height":1080,"fps":30,"bitrate_kbps":8000}
//   {"pxc":1,"kind":"cmd","cmd":"screen_list"}
//   {"pxc":1,"kind":"cmd","cmd":"screen_switch","index":1}
//   {"pxc":1,"kind":"cmd","cmd":"keyframe"}
//
// 被控端的应答：
//   {"pxc":1,"kind":"screens","screens":[{...}]}
//   {"pxc":1,"kind":"video_state","width":..,"height":..,"fps":..,"screen":0,"encoder":"..","error":""}
//
// 键鼠事件（主控端 -> 被控端）：
//   {"pxc":1,"kind":"input","type":"mouse_move","screen":0,"x":0.5,"y":0.5}
//   {"pxc":1,"kind":"input","type":"mouse_button","button":"left","pressed":true}
//   {"pxc":1,"kind":"input","type":"wheel","dy":1}
//   {"pxc":1,"kind":"input","type":"key","key":65,"text":"a","pressed":true,"mods":0}
//
// 所有输入与命令必须通过连接密钥认证后才处理（ClientController 负责把关）。

#include <nlohmann/json.hpp>
#include <string>

namespace pxc::gui {

// 解析一条 ch-control 文本；不是本协议的 JSON 消息时返回 false。
bool parse_session_message(const std::string& text, nlohmann::json& out);

// 生成带标记的 JSON 文本。
std::string encode_session_message(const nlohmann::json& j);

// 常用命令构造器（保持协议常量集中在一处）
nlohmann::json make_video_cfg_cmd(int width, int height, int fps, int bitrate_kbps);
nlohmann::json make_screen_list_cmd();
nlohmann::json make_screen_switch_cmd(int index);
nlohmann::json make_keyframe_cmd();
nlohmann::json make_input_event(const nlohmann::json& payload);

}  // namespace pxc::gui

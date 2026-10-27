//
// receivers.h — 接收者（会话 ID）列表解析工具
//
// receivers.txt / emoji.req 接收者行的共用解析规则（原 raw_pb_broadcast.cpp 与
// emotion_send.cpp 两份同规则拷贝收敛于此，单一实现单一事实源）：
//   '#' 注释 + 空白/逗号/分号分隔的 uint64 会话 ID；0 或残留字符即非法。
//
// 落位：src/config/receivers.{h,cpp}
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 解析接收者文本（receivers.txt 全文，或 emoji.req 接收者行的拼接体）。
// 遇非法 token 整体失败（LOGE 携行号，行号对齐 token 所在行）并清空 out；
// 全部合法时按出现顺序填充。空文本返回 true 且 out 为空，由调用方判空。
bool parse_receivers(const std::string &text, std::vector<uint64_t> &out);

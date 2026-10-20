//
// receiver_utils.cpp — 接收者（会话 ID）列表解析（raw_pb_broadcast / emotion_send 共用）
//
// 解析规则与报错行为沿用 rawpb 原版：'#' 注释 + 空白/逗号/分号分隔的 uint64；
// 非法 token（strtoull 残留字符 / 0）即整体失败，LOGE 行号对齐 token 所在行
// （先 flush 后计数行号）。日志前缀取中性的 [receivers]——调用方各自的
// 外层失败日志（[raw_pb] / [emotion]）补来源上下文。
//
#include "receiver_utils.h"

#include <cstdlib>

#include "logger.h"

bool parse_receivers(const std::string &text, std::vector<uint64_t> &out) {
    out.clear();
    size_t line_no = 1;
    std::string token;

    auto flush = [&]() -> bool {
        if (token.empty()) {
            return true;
        }
        char *end = nullptr;
        const unsigned long long id = std::strtoull(token.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || id == 0) {
            LOGE("[receivers] 第 %zu 行：非法会话 ID \"%s\"", line_no, token.c_str());
            return false;
        }
        out.push_back(static_cast<uint64_t>(id));
        token.clear();
        return true;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '#') {
            if (!flush()) {
                return false;
            }
            while (i + 1 < text.size() && text[i + 1] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == ',' || c == ';') {
            if (!flush()) {
                return false;
            }
            continue;
        }
        if (c == '\n') {
            if (!flush()) {
                return false;
            }
            line_no++; // 先 flush 后计数：报错行号对齐 token 所在行
            continue;
        }
        token.push_back(c);
    }
    return flush();
}

//
// req_config.cpp — 触发配置（*.req）共用文法解析，文法定义见 config/req_config.h
//
#include "config/req_config.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "logger.h"

namespace {

// 累加字段（项目约定：只有 receivers 允许多行分写，其余字段同名取最后一次）
constexpr const char *kAccumulatingKey = "receivers";

// 未出现字段的空串替身（block() 按引用返回，避免调用方无谓拷贝）
const std::string kEmpty;

// 首尾空白（含 CRLF 文件的 '\r'）剥离
void trim(std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

std::string to_lower(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 报错用：把合法字段名拼成 "a / b / c"
std::string join_keys(const std::vector<std::string> &keys) {
    std::string out;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i != 0) out += " / ";
        out += keys[i];
    }
    return out;
}

} // namespace

bool ReqConfig::value(const std::string &key, std::string &out) const {
    const auto it = singles_.find(key);
    if (it == singles_.end() || it->second.empty()) {
        return false;
    }
    out = it->second;
    return true;
}

bool ReqConfig::value_any(const std::vector<std::string> &keys, std::string &out) const {
    for (const std::string &k : keys) {
        if (value(k, out)) {
            return true;
        }
    }
    return false;
}

const std::string &ReqConfig::block(const std::string &key) const {
    const auto it = blocks_.find(key);
    return it == blocks_.end() ? kEmpty : it->second;
}

bool parse_req_config(const std::string &text, const char *config_name,
                      const std::vector<std::string> &known_keys, ReqConfig &out) {
    size_t pos = 0;
    size_t line_no = 1;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        const size_t this_line = line_no;
        if (nl != std::string::npos) {
            ++line_no;
        }

        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // 只切第一个 '='：url=https://x/?a=b&c=d 的值必须整串保留
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOGE("[%s] 第 %zu 行缺 '='（key=value 文法）：%s",
                 config_name, this_line, line.c_str());
            return false;
        }
        std::string key = to_lower(line.substr(0, eq));
        trim(key);  // "url = https://..." 的 key 侧空白同样容错
        std::string value = line.substr(eq + 1);
        trim(value);
        if (key.empty()) {
            LOGE("[%s] 第 %zu 行字段名为空（形如 '=value'）", config_name, this_line);
            return false;
        }

        if (!known_keys.empty() &&
            std::find(known_keys.begin(), known_keys.end(), key) == known_keys.end()) {
            LOGE("[%s] 第 %zu 行未知字段 '%s'（可用：%s）",
                 config_name, this_line, key.c_str(), join_keys(known_keys).c_str());
            return false;
        }

        if (std::strcmp(key.c_str(), kAccumulatingKey) == 0) {
            out.append_block(key, value);
        } else {
            out.set_single(key, value);
        }
    }
    return true;
}

//
// req_utils.h — 触发配置（*.req）共用文法解析
//
// 项目里的 req 文件有两套文法：
//   emoji.req          行式（首行 = 图片路径，其余 = 接收者）—— emotion_send.cpp 自解析
//   link.req/video.req key=value 式 —— 本文件统一承载（单一文法单一实现）
//
// 文法：'#' 注释 / 空行忽略 / key=value 只切【第一个】'='（值里的 '=' 原样保留，
// 带查询串的 URL 必须安全）；key 归一为小写并去空白，值 trim 首尾空白（含 CRLF
// 的 '\r'）。累加约定：`receivers` 可多行分写（各行 '\\n' 拼接），其余字段同名
// 取最后一次。
//
// 严格性：未知 key、无 '='、空 key 一律整体失败并带行号报错——拼错的字段名若被
// 静默丢弃，发出的是"不是我要的内容"，比启动即报错危险得多（与 raw_pb 接收者
// 解析同一立场）。合法字段集由调用方给出（含别名，全部小写）。
//
// 落位：src/config/req_config.{h,cpp}
//
#pragma once
#include <map>
#include <string>
#include <vector>

class ReqConfig {
public:
    ReqConfig() = default;

    // 单值字段（同名多次出现取最后一次）；命中且非空返回 true 并填 out
    bool value(const std::string &key, std::string &out) const;

    // 别名版：按 keys 顺序取第一个命中的非空字段（如 description / desc）
    bool value_any(const std::vector<std::string> &keys, std::string &out) const;

    // 累加字段（receivers）的多行拼接原文；未出现返回空串（交调用方判空/回退）
    const std::string &block(const std::string &key) const;

private:
    friend bool parse_req_config(const std::string &text, const char *config_name,
                                 const std::vector<std::string> &known_keys, ReqConfig &out);

    void set_single(const std::string &key, const std::string &value) { singles_[key] = value; }
    void append_block(const std::string &key, const std::string &value) {
        blocks_[key] += value;
        blocks_[key] += '\n';
    }

    std::map<std::string, std::string> singles_;
    std::map<std::string, std::string> blocks_;
};

// 解析 key=value 触发配置全文。config_name 仅作日志标识（"link.req"），
// known_keys 为该文件承认的字段名（小写，含别名）；空 known_keys = 不校验字段名。
// 返回 false = 文法非法（行号与原因已 LOGE），out 内容不保证完整，调用方应整体弃用。
bool parse_req_config(const std::string &text, const char *config_name,
                      const std::vector<std::string> &known_keys, ReqConfig &out);

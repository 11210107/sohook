//
// Created by user_wangzhen on 2026/8/4.
//
#pragma once
#include <cstdint>
#include <vector>
#include "wwmessage.pb.h"

std::vector<uint8_t> generate_text_message_pb(const std::string& text_content);
std::vector<uint8_t> generate_image_message_pb(
    const std::string &file_path,
    int width,
    int height,
    uint64_t file_size,
    bool is_hd,
    const std::string &thumb_path = "",
    const std::string &mid_thumb_path = "",
    int ld_width = 0,
    int ld_height = 0
);
std::vector<uint8_t> generate_file_message_pb(
    const std::string &file_path,
    uint64_t file_size
);
std::vector<uint8_t> generate_image_reuse_pb(
    const std::string &file_name,
    uint64_t file_size,
    int width,
    int height,
    int ld_width,
    int ld_height,
    const std::string &file_id,
    const std::string &aes_key,
    const std::string &md5
);

std::vector<uint8_t> generate_wwmessage_pb(
    int32_t content_type,
    const std::vector<uint8_t> &content
);

// 图片→表情（contentType=29）：EmotionMessage content PB 组包——字段 1,3,4,5,6,7,8,9,11,12
// 升序、缺省一律不编码（type(2) 即使 GIF 也不带），与 2026-10-16 真机捕获 452 字节实包
// 逐字节一致（宿主单测内嵌该 hex 做字节级回归）。经 proto/gen/ww_emotion_message.pb.h
// 生成类构造；emotion_send 只做编排引用（README §8）。
std::vector<uint8_t> build_emotion_content_pb(
    const std::string &url,           // Tag 1: 主图 lasting URL（兑换 s1）
    const std::string &file_id,       // Tag 3: 上传 fileId 票据
    uint64_t size,                    // Tag 4: 本地文件字节
    const std::string &aes_key,       // Tag 5: CDN AES 密钥
    const std::string &md5,           // Tag 6: 内容 MD5
    uint32_t width,                   // Tag 7: 宽（像素）
    uint32_t height,                  // Tag 8: 高（像素）
    const std::string &description,   // Tag 9（App 收集流实测 "自定义表情"）
    uint32_t source_type,             // Tag 11（2 = EMOTION_USER_ADD）
    const std::string &static_url     // Tag 12: 封面 URL（兑换 s2）
);
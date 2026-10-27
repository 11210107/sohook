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

// 视频消息（contentType=5）：VideoMessage content PB 组包——字段 1,3,4,5,6,7,202
std::vector<uint8_t> build_video_content_pb(
    const std::string &url,               // Tag 1: 本地视频路径（出站本地态）
    uint64_t size,                        // Tag 3: 视频字节数
    uint32_t video_duration,              // Tag 4: 时长（秒）
    uint32_t video_height,                // Tag 5: 高（像素）
    uint32_t video_width,                 // Tag 6: 宽（像素）
    const std::string &preview_img_url,   // Tag 7: 本地缩略图路径（tempimagecache .wwdata；空串 = 不 set 不编码）
    const std::string &raw_url            // Tag 202: 原始地址（实机样例与 url 同值）
);

// 链接卡片消息（contentType=13 = HyperLinkMessage）：LinkMessage content PB 组包
// ——字段 1,2,3,4 升序、空串一律不 set（条件 set，缺省不编码）。纯 URL 三元组
// 即可出站（无文件 IO、无 CDN 上传）；经 proto/gen/wework_link_message.pb.h
// 生成类构造；外层由调用方 generate_wwmessage_pb(13, content) 包装。
std::vector<uint8_t> build_link_content_pb(
    const std::string &link_url,      // Tag 1: 链接地址（必填）
    const std::string &image_url,     // Tag 2: 封面图 URL（空串 = 不 set 不编码）
    const std::string &title,         // Tag 3: 标题（空串 = 不 set 不编码）
    const std::string &description    // Tag 4: 描述（空串 = 不 set 不编码）
);
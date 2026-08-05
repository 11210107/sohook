//
// Created by user_wangzhen on 2026/8/4.
//
#include <string>
#include "message_pb.h"
#include "ww_text_message.pb.h"
#include "wwmessage.pb.h"
#include "ww_rich_message_base.pb.h"
#include "ww_file_message.pb.h"
/**
 * 使用 protobuf 生成类构造文本消息 PB 流
 */
std::vector<uint8_t> generate_text_message_pb(const std::string &text_content) {
    // 1. 最内层 TextMessage
    sohook::WWTextMessage text_msg;
    text_msg.set_content(text_content); // UTF-8 文本内容

    std::string text_msg_bytes;
    text_msg.SerializeToString(&text_msg_bytes);

    // 2. 中间层片段 Message
    sohook::WwRichmessageBase::Message base_msg;
    base_msg.set_content_type(sohook::WwRichmessageBase::CONTENT_TEXT); // 对应 0 (CONTENT_TEXT)
    base_msg.set_data(text_msg_bytes); // 序列化后的 TextMessage[cite: 1]

    // 3. 富文本容器 RichMessage
    sohook::WwRichmessageBase::RichMessage rich_msg;
    *rich_msg.add_messages() = std::move(base_msg); // 存入 messages 数组[cite: 1]

    std::string rich_msg_bytes;
    rich_msg.SerializeToString(&rich_msg_bytes);

    // 4. 最外层 WwMessage::Message
    sohook::Message ww_message;
    ww_message.set_content_type(0);
    ww_message.set_content(rich_msg_bytes); // 序列化后的 RichMessage 存入 content (Tag 10)[cite: 1]

    // 5. 序列化最外层对象为二进制 Buffer
    std::string final_bytes;
    ww_message.SerializeToString(&final_bytes);

    // 6. 转为 std::vector<uint8_t> 返回
    return std::vector<uint8_t>(final_bytes.begin(), final_bytes.end());
}

/**
 * 对应 Java 层的 H1 / d7 逻辑：构建用于企微发送图片的 FileMessage PB
 */
sohook::FileMessage build_file_message_for_image(
    const std::string &file_path,
    int width,
    int height,
    uint64_t file_size,
    bool is_hd,
    const std::string &thumb_path,
    const std::string &mid_thumb_path,
    int ld_width,
    int ld_height
) {
    sohook::FileMessage file_msg;

    // 1. 设置基础文件信息
    file_msg.set_url(file_path); // Tag 3: url

    // 获取文件名 (类似 file.getName())
    std::string file_name = file_path.substr(file_path.find_last_of("/\\") + 1);
    file_msg.set_file_name(file_name); // Tag 2: fileName

    file_msg.set_size(file_size); // Tag 4: size
    file_msg.set_width(width); // Tag 5: width
    file_msg.set_height(height); // Tag 6: height
    file_msg.set_is_hd(is_hd); // Tag 9: isHd

    // 2. 对应 Java 中 P7 == 7 的逻辑：
    // 当消息类型为图片(7)时，设置 voice_time = 7（即 Tag 7，生成 38 07）
    // file_msg.set_voice_time(7); // Tag 7: voiceTime = 7

    // 3. 设置生成的缩略图和中图路径 (对应 H1 里的 psl.d0 缩略图逻辑)
    if (!thumb_path.empty()) {
        file_msg.set_thumbnail_path(thumb_path); // Tag 202: thumbnailPath
    }
    if (!mid_thumb_path.empty()) {
        file_msg.set_mid_thumbnail_path(mid_thumb_path); // Tag 203: midThumbnailPath
    }

    // 4. 设置缩略图宽高 (对应 Java 里的 wechatCdnLdWidth / Height)
    if (ld_width > 0 && ld_height > 0) {
        file_msg.set_wechat_cdn_ld_width(ld_width); // Tag 28: wechatCdnLdWidth
        file_msg.set_wechat_cdn_ld_height(ld_height); // Tag 29: wechatCdnLdHeight
    }

    return file_msg;
}

/**
 * 使用 protobuf 生成类构造图片消息 PB 流
 */
std::vector<uint8_t> generate_image_message_pb(
    const std::string &file_path,
    int width,
    int height,
    uint64_t file_size,
    bool is_hd,
    const std::string &thumb_path,
    const std::string &mid_thumb_path,
    int ld_width,
    int ld_height
) {
    // 1. 构建 Inner FileMessage
    sohook::FileMessage file_msg = build_file_message_for_image(
        file_path, width, height, file_size, is_hd,
        thumb_path, mid_thumb_path, ld_width, ld_height
    );

    std::string file_msg_bytes;
    file_msg.SerializeToString(&file_msg_bytes);


    sohook::Message ww_message;
    ww_message.set_content_type(7); // contentType = 1 (图片)
    ww_message.set_content(file_msg_bytes);

    // 5. 序列化并返回
    std::string final_bytes;
    ww_message.SerializeToString(&final_bytes);

    return std::vector<uint8_t>(final_bytes.begin(), final_bytes.end());
}

/**
 * 使用 protobuf 生成类构造文件消息 PB 流
 */
std::vector<uint8_t> generate_file_message_pb(
    const std::string &file_path,
    uint64_t file_size
) {
    // 1. 构建 Inner FileMessage
    sohook::FileMessage file_msg;

    file_msg.set_url(file_path); // Tag 3: url
    // 获取文件名 (类似 file.getName())
    std::string file_name = file_path.substr(file_path.find_last_of("/\\") + 1);
    file_msg.set_file_name(file_name); // Tag 2: fileName
    file_msg.set_size(file_size); // Tag 4: size

    std::string file_msg_bytes;
    file_msg.SerializeToString(&file_msg_bytes);


    sohook::Message ww_message;
    ww_message.set_content_type(8);
    ww_message.set_content(file_msg_bytes);

    // 5. 序列化并返回
    std::string final_bytes;
    ww_message.SerializeToString(&final_bytes);

    return std::vector<uint8_t>(final_bytes.begin(), final_bytes.end());
}

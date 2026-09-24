//
// Created by user_wangzhen on 2026/8/4.
//
// ★ v4（2026-09-22 第四轮排障）：图片 content_type 7 → 14 ★
//  真机实测（14:31 日志）：v3.1 管线全部达标（主线程发送/单次上传/捕获成功/复用零上传），
//  但 on_result 三连失败：首个 210（消息实际已送达），复用×2 86。
//  根因：type 7 ∉ CDN 类型集合 → LABEL_243 → LABEL_482 直发，复用模型携带的
//  【跨消息 fileid】不经服务器续期直接投递 → 服务端拒绝（86）。
//  type 14 ∈ CDN 集合（sub_5104AB4(14)=1，runtime 实测）→ state 15
//  "renew cdn file"（chat_task.cpp:2682）按 fileid 续期、零文件字节 —— 正是原生
//  转发（sub_1655398 保留 type 14）的合法复用路径。
//  case 13 上传通道按 sub_5104AB4(msgtype) 分流（sub_5106E10.c L2342-2345：
//  CDN 类型→sub_5112730 DoUploadBinWithCdn，否则→sub_51130B4 DoUploadBinWithFtn）。
//  → v4 首传/复用统一 content_type=14：首传票据确定走 CDN 通道，与复用的
//    "renew cdn file" 同通道，全链路票据类型一致。
//  外层 Message.content_type = Tag 7（wwmessage.proto），14 序列化为 "38 0E"。
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
 * v4：content_type=14（原生 CDN 图片类型）。上传通道 case 13 按
 * sub_5104AB4(14)=1 → DoUploadBinWithCdn，票据与复用模型的 state 15
 * "renew cdn file" 续期同通道（v3 用的 7 为另一受支持变体，但无法续期）。
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
    ww_message.set_content_type(7); // v4: 原生 CDN 图片类型（Tag 7，序列化 38 0E）
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
/**
 * v4 复用模型：CDN fileid 免上传直发（方案1）
 *  - file_id/aes_key/md5 来自 OnUploadFinish 捕获的首传 CDN 上传结果；
 *  - url(tag3)/thumbnail(202/203) 刻意留空：路径为空才可达 LABEL_241→LABEL_243
 *    的免上传分支（两者皆空则硬失败 state 4）；
 *  - v4 关键：content_type=14（CDN 类型）→ LABEL_243 → state 15
 *    "renew cdn file" 服务器按 fileid 续期后投递 —— 原生转发同款路径。
 *    （v3 用 7 → LABEL_482 直发，跨消息 fileid 不续期 → 实测 errcode 86 被拒）
 */
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
) {
    sohook::FileMessage file_msg;
    file_msg.set_file_id(file_id);        // Tag 1:  fileid（服务器文件票据，免上传判定键）
    file_msg.set_file_name(file_name);    // Tag 2:  文件名
    // Tag 3 (url/本地路径) 刻意留空 —— 留空即复用，填了会进上传子树
    file_msg.set_size(file_size);         // Tag 4:  文件大小
    file_msg.set_width(width);            // Tag 5
    file_msg.set_height(height);          // Tag 6
    file_msg.set_aes_key(aes_key);        // Tag 8:  CDN AES key
    file_msg.set_md5(md5);                // Tag 10: 内容 MD5
    if (ld_width > 0 && ld_height > 0) {
        file_msg.set_wechat_cdn_ld_width(ld_width);   // Tag 28: 缩略图宽
        file_msg.set_wechat_cdn_ld_height(ld_height); // Tag 29: 缩略图高
    }
    // Tag 202/203（本地缩略图路径）刻意不带：不带才不触发第二媒体部件(state 20)上传

    std::string file_msg_bytes;
    file_msg.SerializeToString(&file_msg_bytes);

    sohook::Message ww_message;
    ww_message.set_content_type(14);      // v4: CDN 图片类型 → LABEL_243 → state 15 "renew cdn file" 续期
    ww_message.set_content(file_msg_bytes);

    std::string final_bytes;
    ww_message.SerializeToString(&final_bytes);
    return std::vector<uint8_t>(final_bytes.begin(), final_bytes.end());
}

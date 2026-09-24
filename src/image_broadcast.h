//
// image_broadcast.h — 同一张图片群发多联系人，CDN 只上传一次（方案1：fileid 免上传）
//
// 时序：
//   1. 首个联系人：常规发送（模型带本地路径 → 状态机 LABEL_29→LABEL_449 → state 13
//      → DoUploadBinWithFtn 上传一次；cdn_cache 的 OnUploadFinish hook 捕获
//      fileid/aeskey/md5）
//   2. 其余联系人：构建"复用模型"（FileMessage 带 file_id/aes_key/md5、不带 url），
//      状态机 case 10 走 LABEL_241(fileid 非空)→LABEL_243(msgtype=7 非 CDN 集合)
//      →LABEL_482 直发（state 保持 20/21 → case 21/22 协议发送），零上传
//   3. 缓存未命中/等待超时：自动回退为逐个常规发送（退化为旧行为，不会漏发）
//
// 落位：src/image_broadcast.h
//
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using BroadcastProgress = std::function<void(int64_t, int64_t, uint64_t)>;
using BroadcastResult = std::function<void(int, uint64_t)>;

// 内部自起 detached 线程执行（等待 CDN 上传结果可能耗时数秒~数十秒，
// 绝不能阻塞调用方/主线程），立即返回
void send_image_to_contacts(const std::string &image_path,
                            const std::vector<uint64_t> &receiver_conv_ids,
                            BroadcastProgress onProgress,
                            BroadcastResult onResult);

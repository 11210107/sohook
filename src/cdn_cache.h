//
// cdn_cache.h — CDN 上传结果捕获与复用缓存（方案1：fileid 免上传）
//
// 原理：hook CdnUploadTask::OnUploadFinish（0x2B88B70，CDN/FTN 两通道所有上传完成的
// 唯一汇聚点，见 file_msg_upload_cdn.md §9.5），上传成功时把 (fileid票据, aeskey, md5)
// 按本地文件路径缓存。后续对同一文件的发送改用"复用模型"（FileMessage 带
// file_id/aes_key/md5、不带本地路径），ChatTask case 10 走 LABEL_241(fileid非空)
// →LABEL_243 按【内部 msgtype(task+584)】分流 → state 15 "renew cdn file" 续期
// （零文件字节）。注意：模型外层 content_type 与内部 msgtype 存在 native 侧映射
// （真机实测外层 7 带路径消息的 task+584=14，映射点未定位；v4 起首传/复用统一
// 外层 14，真机三投全送达）。v4.1 增设状态机观测 hook（[chat_task] dispatch 日志），
// 逐次打印 state/msgtype/err，用于验证映射与解码 on_result 错误码。
//
// 落位：src/cdn_cache.h
//
#pragma once
#include <cstdint>
#include <string>

struct CdnUploadResult {
    std::string file_id;      // FileMessage tag 1（wwftn 长票据，"3069..." 十六进制串）
    std::string aes_key;      // FileMessage tag 8（32 hex）
    std::string md5;          // FileMessage tag 10（32 hex）
    int64_t captured_at_ms = 0;
};

// 安装捕获 hook + v4.1 状态机观测 hook（libwework_framework.so 基址就绪后调用一次，幂等；
// 观测 hook 失败只少日志，不影响捕获功能，也不影响本函数返回值）
bool init_cdn_capture_hooks(uintptr_t so_base);

// 查询捕获 hook 是否已安装（广播层据此决定是否做 60s 等待；未装则全部常规发送）
bool cdn_cache_hooks_installed();

// 查询/登记（TTL 过期视为未命中，避免复用过期 fileid）
bool cdn_cache_get(const std::string &local_path, CdnUploadResult &out);
void cdn_cache_put(const std::string &local_path, const CdnUploadResult &r);

// 阻塞等待（供广播线程轮询首个接收者的上传结果，勿在主线程调用）
bool cdn_cache_wait(const std::string &local_path, int timeout_ms, CdnUploadResult &out);

// 验证辅助：该路径触发的 CdnUploadTask::Start 次数（"只上传一次"的量化证据）
int cdn_upload_start_count(const std::string &local_path);

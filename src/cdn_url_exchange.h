//
// gif_url_exchange.h — 独立 CDN 上传 + 服务端换 URL（复刻自定义表情 ylj.d0 链路）
//
// App 侧真实链路（trace/addCustomEmojiInfo.md 实测）：
//   ylj.d0 → FileService.CdnUploadFileOnPath(path, 1, cb, prog, 42)
//        → nativeCdnUploadFileOnPathSceneAndSubScene(0x11EA680)
//        → onResult 拿 (fileId/aesKey/md5)          ← 上传半程，无消息副作用
//   CollectionProtocol.getGifUrlByFileId(fileId, aeskey, md5, cb)  (0x10B6838)
//        → CollectionBackend::GetGifUrlByFileId (0x50C35D8)
//        → "get_gif_url_by_fileid_task" CGI(CMD 2044, GetTegLastingUrlReq)
//        → onResult(errcode, s1, s2, s3)             ← 兑换半程，URL 由服务端签发
//
// sohook 复刻形态：
//   1) 独立上传：SM→vt+48 取 FileService → vt+192(request*, 空result_ref, 空progress_ref)
//      —— 与 JNI 空回调分支完全同构（App 真实支持 null 回调对象）；上传完成不由我们
//      伪造回调观测，而是复用既有 cdn_cache 的 OnUploadFinish 观测 hook 被动捕获
//      (fileid/aeskey/md5)（RPC 服务端实现 sub_50C6934 同样创建 CdnUploadTask，
//      OnUploadFinish 汇聚点覆盖本通道）。
//   2) 服务端换 URL：SM→vt+376(Collection 服务)→vt+112(Backend)→
//      vt+440(fileId&, aeskey&, md5&, ClosureRef&)，伪造 0x40 完成闭包（构造走框架
//      自有 ctor sub_6D35F44，引用计数/destroy 协议与原生闭包一致）。
//      完成回调 invoke ABI（sub_2860C54 实证）：
//        int64_t invoke(closure, int* errcode, str1*, str2*, str3*)
//      errcode 按指针传入；三个 libc++ string 归 deferred 闭包所有——invoke 只读
//      （read_native_string），绝不释放；闭包引用计数归零时 destroy 收尾。
//   3) 回调模式（可选，url.req 注释行 mode=cb / mode=callback）：vt+192 递交伪造
//      result/progress 闭包（JNI 非空回调分支同构；invoke ABI 见 cpp 头注），完成
//      回调直接投递主线程兑换任务——无共享状态、无轮询、不经 [cdn_cache] 取结果。
//
// 线程模型（对齐 raw_pb_broadcast / image_broadcast v3.1+ 的既有教训）：
//   * 入口在 my_nativeSend（主线程）触发：解析文件 + initialize MainThreadExecutor；
//   * 一切原生调用（vt+192 / 兑换链）经 MainThreadExecutor 投递主线程——天然排在
//     orig_nativeSend 之后，避免在发送管线持有锁时重入；
//   * 等待上传（60s 轮询 + Start 计数心跳）留 detached 线程，绝不阻塞主线程；
//   * 完成闭包在框架回调线程被调：只读字符串 + 日志 + 写 url.result，无 JNI、无锁。
//
// 落位：src/gif_url_exchange.{h,cpp}
//
#pragma once
#include <cstdint>
#include <functional>
#include <string>

// 兑换结果回调：errcode 0 = 成功；s1/s2/s3 = GetTegLastingUrlResponse 的三个字符串
// （2026-10-16 真机定名：s1=主URL、s2=封面/缩略图URL（独立资产ID、同签发时间戳）、
//  s3=md5 回显——与上传捕获的 md5 逐字节一致）
using GifUrlResult = std::function<void(int errcode,
                                        const std::string &new_url,
                                        const std::string &cover_url,
                                        const std::string &md5)>;

// 全量结果回调（图片→表情等需要在兑换完成后继续组包的调用方）：errcode 0 = 成功；
// file_id/aes_key/md5 = 上传三元组随回调一并回传（调用方无须另存共享状态）；
// s1/s2/s3 = 兑换三串（语义同上注）。上传半程失败以 errcode=-1 合成交付（三串为空）。
using GifUrlFullResult = std::function<void(int errcode,
                                            const std::string &file_id,
                                            const std::string &aes_key,
                                            const std::string &md5,
                                            const std::string &s1,
                                            const std::string &s2,
                                            const std::string &s3)>;

// 完整链路：独立上传拿 fileId → 服务端换 URL。立即返回，结果异步经 onResult 交付。
// scene/subscene/type = 上传请求三 int（App 表情流实测 (1,42,-1)，见 addCustomEmojiInfo.md）。
void upload_and_exchange_gif_url(const std::string &local_path,
                                 int scene, int subscene, int type,
                                 GifUrlResult onResult);

// 回调模式变体（观测模式之上的可选项，url.req 注释行 mode=cb 选用）：上传完成
// 不经 [cdn_cache] 观测 hook/缓存取结果，而是向 vt+192 递交伪造 result/progress
// 闭包（JNI 非空回调分支同构）——完成回调直接投递主线程兑换任务（纯回调链路，
// 无共享状态、无轮询）。url.result 落盘与观测模式一致。
void upload_and_exchange_gif_url_cb(const std::string &local_path,
                                    int scene, int subscene, int type,
                                    GifUrlResult onResult);

// 回调模式的全量变体（图片→表情发送链路）：链路与 cb 变体完全一致（纯回调、无共享
// 状态、无轮询、不经 [cdn_cache]），唯完成时上传三元组 + 兑换三串七元组一次交付——
// 组 content PB 需要的 file_id/aes_key/md5 不在 GifUrlResult 交付物里，故设此变体。
void upload_and_exchange_gif_url_full(const std::string &local_path,
                                      int scene, int subscene, int type,
                                      GifUrlFullResult onResult);

// 文件驱动一次性入口（my_nativeSend 主线程触发，返回 true = 本次触发已由本模块接管）：
//   <cache>/url.req   —— 首个非注释行 = 本地文件绝对路径（可缺省 =
//                        /storage/emulated/0/test.jpg，空文件即“上传默认图换URL”）；
//                        可选第二个非注释行 "scene,subscene,type"（缺省 1,42,-1）；
//                        任一注释行含 mode=cb / mode=callback → 回调模式（§回调闭包）
//   装载成功即改名 url.req.sent（防手动连发重复上传）；兑换结果落 <cache>/url.result
//   （errcode + 三串，adb pull 即取）。文件不存在返回 false，调用方走原逻辑。
bool try_exchange_gif_url_from_files();

//
// raw_pb_send.h — 直接 PB 流群发编排（视频号/小程序等捕获消息的重放）
//
// 与 generate_*_pb 拼装发送并列的另一种发消息形式：把拦截捕获的完整
// Message PB（framework.js nativeSetInfo 完整打印的 hex 流）清洗后原样
// 投给 vtable+848 发送管线。PB 内容自包含（URL/文本，无本地文件），
// 不需要 CDN 捕获/等待——这是与 send_image_to_contacts 的核心差异。
//
// 线程模型与 image_broadcast v3.1/v4 完全一致（sohook_patch README §2.1 教训）：
//   * 发送原语 vtable+848 必须回【主线程】执行（经 MainThreadExecutor 投递）；
//   * 编排（节流 50ms/条）留 detached 后台线程，绝不阻塞主线程；
//   * 入口带主线程护栏，由 my_nativeSend（主线程实测）触发。
//
// 落位：src/types/raw_pb_send.{h,cpp}
//
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

using BroadcastProgress = std::function<void(int64_t, int64_t, uint64_t)>;
using BroadcastResult   = std::function<void(int, uint64_t)>;

// 直接 PB 流群发。raw_pb 应为 rawpb::sanitize_message_pb_for_send 的产物
//（或经 rawpb::validate_message_pb 校验的 #keep-all 原始流）。立即返回，
// 实际发送逐条投递到主线程异步执行。
void send_raw_pb_to_contacts(const std::vector<uint8_t> &raw_pb,
                             const std::vector<uint64_t> &receiver_conv_ids,
                             BroadcastProgress onProgress,
                             BroadcastResult onResult);

// 便捷入口：设备存储配置驱动（无需改代码重编译即可换 PB / 换接收人）。
//   /storage/emulated/0/sohook/pb.hex        —— hex 文本流（'#' 注释；任意
//                                               位置写 #keep-all 则跳过清洗
//                                               原样重放）
//   /storage/emulated/0/sohook/pb.bin        —— 原始二进制 PB 字节（捕获工
//                                               具落盘的 .bin 直接 adb push，
//                                               如工程 file/image_message.bin；
//                                               无注释通道，固定走默认清洗，
//                                               要 #keep-all 先转 hex：
//                                               xxd -p pb.bin > pb.hex）
//   /storage/emulated/0/sohook/receivers.txt —— 会话 ID 列表（'#' 注释，
//                                               空白/逗号/分号分隔的 uint64）
// 返回 true = pb.hex 或 pb.bin 存在、本次触发已由 raw PB 模式接管（即使后续
// 解析失败也不再走图片测试路径，防止双发；两格式并存同样接管但拒绝发送）；
// false = 两者都不存在，调用方走原逻辑。
// 装载成功后来源文件改名为 *.sent（一次性触发，防手动连发重复群发；
// 改名失败仅告警不阻断——下次触发会再发一轮，注意手动删除）。
bool try_send_raw_pb_from_files(BroadcastProgress onProgress,
                                BroadcastResult onResult);

// wwmessage_db_call.h — DB 三字段(content / extraContent / content_type)驱动的
// 视频号消息群发：generate_wwmessage_pb 的调用示范，与 raw_pb 文件通道互不影响。
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// 由 DB 消息行三列重构出站 PB 并群发（返回是否成功启动发送）。
// id_list 为目标会话句柄；onProgress/onResult 语义同 send_model_message。
bool send_wwmessage_from_db(const std::vector<uint64_t> &id_list,
                            std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                            std::function<void(int, uint64_t)> onResult);

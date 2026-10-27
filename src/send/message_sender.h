//
// Created by user_wangzhen on 2026/5/19.
//

#ifndef SOHOOK_MESSAGE_SENDER_H
#define SOHOOK_MESSAGE_SENDER_H
#include <stdint.h>
#include <functional>
#include <vector>
// 消息类型，对应企微底层 msg_type
constexpr int MSG_TYPE_TEXT = 0;
constexpr int MSG_TYPE_IMAGE = 7;
constexpr int MSG_TYPE_FILE = 8;
int64_t send_model_message(uint64_t target_conv_id, int msg_type, std::vector<uint8_t> pb_data,
                           std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                           std::function<void(int, uint64_t)> onResult);
#endif //SOHOOK_MESSAGE_SENDER_H

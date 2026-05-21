//
// Created by user_wangzhen on 2026/5/19.
//

#ifndef SOHOOK_WEWORK_CONVERSATION_SERVICE_H
#define SOHOOK_WEWORK_CONVERSATION_SERVICE_H
#include <stdint.h>
#include "wework_message_factory.h"
int64_t send_model_message(uint64_t target_conv_id,const MessageParam& param);
#endif //SOHOOK_WEWORK_CONVERSATION_SERVICE_H
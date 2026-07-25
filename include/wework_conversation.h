//
// Created by user_wangzhen on 2026/5/16.
//

#ifndef SOHOOK_WEWORK_CONVERSATION_H
#define SOHOOK_WEWORK_CONVERSATION_H
#include <stdint.h>
void* create_and_inject_conversation(uint64_t target_conv_id);
void* get_cache_conversation_by_key_native(uint32_t conv_type, uint64_t remote_id);
#endif //SOHOOK_WEWORK_CONVERSATION_H
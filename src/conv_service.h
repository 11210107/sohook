//
// Created by user_wangzhen on 2026/9/9.
//
# pragma once
#include <cstdint>
uintptr_t getConversationService();

void* get_cache_conversation_by_key(uint32_t conv_type, uint64_t remote_id) ;
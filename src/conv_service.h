//
// Created by user_wangzhen on 2026/9/9.
//
# pragma once
#include <cstdint>
#include "conv_type.h"
uintptr_t getConversationService();

void* get_cache_conversation_by_key(ConvType conv_type, uint64_t remote_id) ;
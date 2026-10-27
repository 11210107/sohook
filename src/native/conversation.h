//
// Created by user_wangzhen on 2026/9/9.
//
#pragma once
#include <stdint.h>
#include "native/conv_type.h"
ConvType resolve_conv_type(uint64_t conversation_id);
void *create_native_conversation(uint64_t remote_id);

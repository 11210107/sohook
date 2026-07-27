//
// Created by user_wangzhen on 2026/7/27.
//

#pragma once

#define OFFSET_JAVA_VM                      (0x92520D8) // java_vm

#define OFFSET_NATIVE_GET_CURRENT_PROFILE   (0xF9B320)  // nativeGetCurrentProfile
#define OFFSET_NATIVE_VID                   (0x11E3DC8) // nativeVid

#define OFFSET_MSG_SEND                     (0x25C9374) // 消息发送
#define OFFSET_MSG_SEND_CB                  (0x5E9D49C) // 消息发送闭包回调
#define OFFSET_ADD_REF                      (0x5EB5470) // 引用增加
#define OFFSET_DEC_REF                      (0x5EB5458) // 引用减少
#define OFFSET_CREATE_CONV                  (0x137E690) // 创建空会话
#define OFFSET_INIT_PB                      (0x1363110) // 初始化PB
#define OFFSET_PARSE_FROM_MEM               (0x5F64AC4) // 从内存解析
#define OFFSET_SYNC_TO_CONV                 (0x1365C18) // 同步到会话
#define OFFSET_DESTROY_PB_STRUCT            (0x1363848) // 销毁PB结构体
#define OFFSET_CREATE_MSG_PTR               (0x137E7C4) // 创建消息指针
#define OFFSET_INIT_CORE_PTR                (0x130530C) // 初始化消息对象核心指针
#define OFFSET_COPY_CORE_PTR                (0x1307C5C) // 拷贝消息对象核心指针
#define OFFSET_INIT_CONV_KEY                (0x126D9D4) // 初始化会话key
#define OFFSET_FIND_CONV_BY_CACHE           (0x258B330) // 根据会话key查找会话



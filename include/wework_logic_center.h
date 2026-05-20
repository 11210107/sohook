//
// Created by user_wangzhen on 2026/5/15.
//

#ifndef SOHOOK_WEWORK_LOGIC_CENTER_H
#define SOHOOK_WEWORK_LOGIC_CENTER_H
#include <stdint.h>
/**
 * 获取当前活跃的 ConversationService 指针
 * 寻址链路: ProfileManager -> ActiveProfileMgr -> Profile -> Logic -> ConversationService
 *
 * @return 成功返回对象指针，失败返回 0
 */
uintptr_t getCurrentConvService();


#endif //SOHOOK_WEWORK_LOGIC_CENTER_H
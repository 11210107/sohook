//
// Created by user_wangzhen on 2026/10/11.
//
#pragma once

#include <cstdint>

// 会话类型 convType，取值与 libwework_framework 中 CONVTYPE 定义一一对应：
//   0       CONVTYPE_PERSON                          单聊（个人会话）
//   1       CONVTYPE_ROOM                            群聊
//   2       CONVTYPE_ANNOUNCE                        公告
//   3       CONVTYPE_APP                             应用会话
//   4       CONVTYPE_FILE_ASSISTANT                  文件传输助手
//   5       CONVTYPE_CUSTOMER_SERVICE                客服会话
//   6       CONVTYPE_OPENAPI_APP                     OpenAPI 应用
//   7       CONVTYPE_CONVERGE                        聚合会话
//   9       CONVTYPE_WX_PERSON                       微信单聊
//   10      CONVTYPE_XCX                             小程序
//   11      CONVTYPE_WEIXIN_CHAT                     微信聊天
//   101     CONVTYPE_BLACKBOARD_CHANNEL_TYPE         黑板频道
//   103     CONVTYPE_DOC                             文档
//   104     CONVTYPE_MEETING                         会议
//   105     CONVTYPE_SYS_MEETING_NOTIFY              系统会议通知
//   10000   CONVTYPE_CUSTOMER_MASS_MESSAGE_FAKE      客户群发（假会话）
//   10001   CONVTYPE_WX_OPENSDK_FORWARD_MESSAGE_FAKE 微信 OpenSDK 转发（假会话）
//   9999999 CONVTYPE_INVALID                         无效/未设置（哨兵值）
enum class ConvType : uint32_t {
    kPerson = 0,                      // CONVTYPE_PERSON：单聊（个人会话）
    kRoom = 1,                        // CONVTYPE_ROOM：群聊
    kAnnounce = 2,                    // CONVTYPE_ANNOUNCE：公告
    kApp = 3,                         // CONVTYPE_APP：应用会话
    kFileAssistant = 4,               // CONVTYPE_FILE_ASSISTANT：文件传输助手
    kCustomerService = 5,             // CONVTYPE_CUSTOMER_SERVICE：客服会话
    kOpenApiApp = 6,                  // CONVTYPE_OPENAPI_APP：OpenAPI 应用
    kConverge = 7,                    // CONVTYPE_CONVERGE：聚合会话
    kWxPerson = 9,                    // CONVTYPE_WX_PERSON：微信单聊
    kXcx = 10,                        // CONVTYPE_XCX：小程序
    kWeixinChat = 11,                 // CONVTYPE_WEIXIN_CHAT：微信聊天
    kBlackboardChannel = 101,         // CONVTYPE_BLACKBOARD_CHANNEL_TYPE：黑板频道
    kDoc = 103,                       // CONVTYPE_DOC：文档
    kMeeting = 104,                   // CONVTYPE_MEETING：会议
    kSysMeetingNotify = 105,          // CONVTYPE_SYS_MEETING_NOTIFY：系统会议通知
    kCustomerMassMessageFake = 10000, // CONVTYPE_CUSTOMER_MASS_MESSAGE_FAKE：客户群发（假会话）
    kWxOpenSdkForwardMessageFake = 10001, // CONVTYPE_WX_OPENSDK_FORWARD_MESSAGE_FAKE：微信 OpenSDK 转发（假会话）
    kInvalid = 9999999,               // CONVTYPE_INVALID：无效/未设置（哨兵值）
};

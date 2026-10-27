# SoHook

在 Native 层实现 C++方法的 inline hook，拦截（目标 app）发消息方法，并实现 Naive 层发送消息功能。

### build

idea 添加Cmake交叉编译Profiles

```
-DCMAKE_TOOLCHAIN_FILE=/Users/user/Library/Android/sdk/ndk/25.1.8937393/build/cmake/android.toolchain.cmake
-DANDROID_ABI=arm64-v8a 
-DANDROID_PLATFORM=android-21 
-DANDROID_STL=c++_static
```

### 部署

执行 scripts/push_so.sh 命令，部署 libsohook.so 到手机。

### 目标 app（Wework）

* 1.在 C++ 层面构造好你的图片 PB 结构体
* 2.直接寻找底层 foundation::Message::SetInfo(const std::string& data) 或类似的 C++ 函数。
* 3.直接把 PB 序列化后的数据喂给它。

方案 B：拦截并篡改 (操作字节流)
如果你非要搞 PB 二进制流：

Hook nativeSetInfo。

观察图片消息发送时传入的 a4 (byte 数组)。

你会发现这是一个标准的 Protobuf 格式。

图片消息的 ContentType 通常是 2。

### Protobuf 序列化

* protobuf库版本查看：thirdparty/protobuf/arm64-v8a/include/google/protobuf/stubs/common.h
* #define GOOGLE_PROTOBUF_VERSION 7035001
* protoc 本地版本使用 3.35.1 /opt/homebrew/bin/protoc
* 根据描述文件生成 C++ 的类实现（.pb.cc）和类声明（.pb.h） /Users/user/CLionProjects/sohook/opt/homebrew/bin/protoc
  --proto_path=proto --cpp_out=proto/gen wework_conversation.proto wework_text_message.proto wework_image_message.proto
* /opt/homebrew/bin/protoc --proto_path=proto --cpp_out=proto/gen ww_file_message.proto

# 构建 WwMessage.Message

public WwMessage.Message buildWwMessage(String text) {
// 1. 最内层 TextMessage
sohook.TextMessage textMsg = new sohook.TextMessage();
textMsg.content = text.getBytes(StandardCharsets.UTF_8); // "如" -> e5 a6 82

    // 2. 中间层片段 Message
    sohook.Message baseMsg = new sohook.Message();
    baseMsg.contentType = sohook.Message.ContentType.TYPE_0; // 08 00
    baseMsg.data = MessageNano.toByteArray(textMsg);         // 12 05 0a 03 e5 a6 82

    // 3. 富文本容器 RichMessage
    WwRichmessageBase.RichMessage richMsg = new WwRichmessageBase.RichMessage();
    richMsg.messages = new sohook.Message[]{ baseMsg };     // 0a 09 ...

    // 4. 最外层 WwMessage.Message
    WwMessage.Message wwMessage = new WwMessage.Message();
    wwMessage.contentType = 0;
    wwMessage.content = MessageNano.toByteArray(richMsg);    // 52 0b ...

    return wwMessage;

}

### ConversationService 对象获取（企业微信版本 5.0.10）

```
public static ConversationService getService() {
    return Application.getInstance().GetProfileManager().GetCurrentProfile().getServiceManager().GetConversationService();
}
```

|         **JNI 函数名**          |              **Java 对应方法**              |             **Native 内部逻辑**             |       **虚表偏移**       |                    **作用**                    | 
|:----------------------------:|:---------------------------------------:|:---------------------------------------:|:--------------------:|:--------------------------------------------:|
|   nativeGetCurrentProfile    |   ProfileManager.GetCurrentProfile()    |       *(*(*v0+24)+24) + new(0x10)       |  vtable + 24 (0x18)  |  从单例中获取 Profile 并包装成 16 字节 Handle 返回给 Java   |
|   nativeGetServiceManager    |       Profile.getServiceManager()       | v3 = *a3;          (*(*v3 + 264LL))(v3) | vtable + 264 (0x108) |   解引用句柄 a3，调用 Profile 虚表获取 ServiceManager    |
| nativeGetConversationService | ServiceManager.GetConversationService() |           (*(*a3 + 40LL))(a3)           |  vtable + 40 (0x28)  | 直接使用 ServiceManager 虚表获取 ConversationService |

## src 目录组织（目标结构，按流水线层次而非文件类型）

> **状态（2026-10-27）**：目录迁移**已落地**——`src/` 根只剩 `main.cpp` + `offset.h`，其余按下列层次分目录；
> include 全部改为工程相对路径（`include_directories(src)`，禁 `../`），`CMakeLists.txt` 改为按目录
> `GLOB_RECURSE ... CONFIGURE_DEPENDS` 收集源文件（新增/移动文件不再需要改构建脚本，收录清单与原显式
> 列表 24 个 src + 10 个 proto/gen 逐一对齐）。重命名到位：`image_broadcast→types/image_send`、
> `raw_pb_broadcast→types/raw_pb_send`、`wwmessage_db_call→types/db_message_send`、
> `utils/req_utils→config/req_config`、`utils/receiver_utils→config/receivers`。
>
> **尚未做的部分**（★ 标记 = 待新建文件，都是"收敛已有重复"的产物，不是新发明的抽象）：
> `hook/cdn_capture`（从 cdn_cache 拆出 4 个 DobbyHook）、`native/closure`（从 message_sender 抽出闭包伪造）、
> `send/broadcast`（5 份群发编排合一）、`config/paths.h`（5 份 `*_CONFIG_DIR` 宏合一）、
> `runtime/thread_utils`（6 份 `current_tid/is_main_thread` 合一）、`media/cdn_upload`（cdn_url_exchange 拆分）、
> `types/text_send`、`types/file_send`（目前只是 message_pb 里的生成函数，无独立编排入口）；
> `utils/protocol_utils.cpp`（第一代死码）的处置未做。
> （`include/` 瘦身已完成：`dobby.h` → `thirdparty/dobby/include/`、
> `logger.h`/`vtable_helper.h` → `src/utils/`，工程根不再有 `include/`。）

```text
src/
├── main.cpp                     so constructor：只做"启动 hook"
├── offset.h                     原生偏移 + 内存布局单点（版本升级只动这里）
│
├── hook/                        拦截面（唯一知道"企微在哪被拦"的地方）
│   ├── send_hook.{h,cpp}        nativeSendModelMessage 拦截 + 分发到 types/
│   └── cdn_capture.{h,cpp}      ← 从 cdn_cache.cpp 拆出的 4 个 DobbyHook
│
├── native/                      ABI 伪造层：所有"手写企微对象内存布局"都在这
│   ├── conv_type.h              （原 src/conv_type.h）
│   ├── conv_service.{h,cpp}     ConversationService 寻址 + 缓存会话查找
│   ├── conversation.{h,cpp}     会话句柄创建
│   ├── contact_service.{h,cpp}
│   ├── msg_ptr.{h,cpp}          消息句柄 ← 原 message/msg_ptr
│   └── closure.{h,cpp}          Progress/Result 闭包伪造 ← 从 message_sender.cpp 抽出
│
├── message/                     消息模型（纯函数、零原生调用 → 最适合宿主单测）
│   ├── message_pb.{h,cpp}
│   └── raw_pb.{h,cpp}
│
├── send/                        发送：一个原语 + 一个编排
│   ├── message_sender.{h,cpp}   send_model_message（全工程唯一发射口）
│   └── broadcast.{h,cpp}        ★群发编排（detached 节流 + 主线程投递），收 5 份拷贝
│
├── types/                       一种消息类型一个文件，每个都很薄
│   ├── text_send.{h,cpp}        ★待建（现仅 message_pb 的 generate_text_message_pb，无编排入口）
│   ├── image_send.{h,cpp}       ← image_broadcast（含 CDN 票据复用模型）
│   ├── file_send.{h,cpp}        ★待建（同上，仅有 generate_file_message_pb）
│   ├── emotion_send.{h,cpp}
│   ├── video_send.{h,cpp}
│   ├── link_send.{h,cpp}
│   ├── raw_pb_send.{h,cpp}      ← raw_pb_broadcast
│   └── db_message_send.{h,cpp}  ← wwmessage_db_call
│
├── media/                       素材与上传
│   ├── cdn_cache.{h,cpp}        只留"票据缓存 + 查表"（hook 部分拆给 hook/cdn_capture）
│   ├── cdn_upload.{h,cpp}       ← cdn_url_exchange 的上传半程
│   └── cdn_url_exchange.{h,cpp} 兑换半程（Collection CGI）
│
├── config/                      触发配置（*.req）
│   ├── paths.h                  ★设备目录/文件名常量单点（干掉 5 份 *_CONFIG_DIR 宏）
│   ├── req_config.{h,cpp}       ← utils/req_utils（key=value 文法 + 字段白名单）
│   └── receivers.{h,cpp}        ← utils/receiver_utils
│
├── runtime/                     执行环境
│   ├── main_thread_executor.{h,cpp}
│   ├── zygisk_inject.cpp        ← src/zygisk_inject.cpp
│   └── thread_utils.{h,cpp}     ★current_tid / is_main_thread（收 6 份拷贝）
│
└── utils/                       真·杂项（不含业务语义）
    ├── address_utils.{h,cpp}
    ├── file_utils.{h,cpp}
    ├── logger.h                 ✅原 include/logger.h（LOGD/LOGI/LOGW/LOGE 宏）
    ├── vtable_helper.h          ✅原 include/vtable_helper.h（手写虚表调用，无偏移知识）
    └── protocol_utils.{h,cpp}   第一代死码 → 建议删除或移入 legacy/
```

✅ **`include/` 已瘦身完毕**（2026-10-27）：工程根 `include/` 目录删除。
第三方头 `dobby.h` → `thirdparty/dobby/include/`（与 `libdobby.a` 同仓；放架构无关的
`include/` 而非 `arm64-v8a/` 下，将来补别的 ABI 时头共用一份），调用点**仍写裸名**
`#include "dobby.h"`——第三方头不进工程相对路径体系，与 protobuf 一致。
项目自己的 `logger.h`、`vtable_helper.h` → `src/utils/`，调用点改为
`#include "utils/logger.h"`（24 处）、`#include "utils/vtable_helper.h"`（4 处）。

### 配套规范（让目录结构可执行）

1. **include 一律工程相对路径**：`CMakeLists.txt` 加 `include_directories(src)`，于是全部写成 `#include "native/conv_service.h"`、`#include "config/paths.h"`；禁止 `../` 写法（现存 0 处）。例外：第三方头（`dobby.h`、protobuf）保持裸名，它们的搜索路径由 `thirdparty/**/include` 提供。
2. **依赖方向单向**：`hook → types → send → {message, native, media} → {config, runtime, utils}`。反向即违规：`native/` 不得 include `types/`，`utils/` 不得 include 任何业务头。
3. **types/ 每类固定两个入口**：`try_send_<type>_from_files(...)`（生产接线，文件驱动；配置不存在返回 `false` = 该类型未启用）+ `send_<type>_to_contacts(...)`（参数直传）。回调一律 `std::function<void(int64_t, int64_t, uint64_t)> onProgress, std::function<void(int, uint64_t)> onResult` 直写（`link_send` / `video_send` 已是此形）。

### 迁移顺序（先解耦构建脚本，再动文件）

1. ⏳ `git add` + 提交当前工作区（勿把重命名与逻辑改动混在一次提交）。**本次迁移未提交**，
   工作区里同时含功能改动与目录改动，建议分两个提交。
2. ✅ `CMakeLists.txt` 改按目录收集，此后移动文件不再需要改构建脚本：
   ```cmake
   file(GLOB_RECURSE SOHOOK_SOURCES CONFIGURE_DEPENDS src/*.cpp proto/gen/*.cc)
   target_sources(sohook PRIVATE ${SOHOOK_SOURCES})
   include_directories(src)
   ```
3. ✅ `git mv` 按目录整批移动（纯重命名，零逻辑改动）+ 一次性统一 include 前缀。
   把关：全新配置构建 34/34 TU 通过（`cmake-build-verify` 已删）、`cmake-build-android` 全量重建
   38 步通过、`llvm-nm` 确认 `send_model_message`/`try_send_*_from_files`/`parse_req_config` 等符号在 `.so` 内。
4. ✅ 部分：文件头自述与"落位"路径已改到 `cdn_url_exchange`；仍写 `gif_url_exchange` 的是
   `GIF_URL_CONFIG_DIR` 宏名（emotion/cdn 两处共用）与 `url.result` 的首行输出串
   （`media/cdn_url_exchange.cpp` 的 fprintf）——后者是设备可见文本，留待 `config/paths.h` 一并处理。
5. ✅ `include/` 瘦身：`dobby.h` → `thirdparty/dobby/include/`（CMake 换搜索路径，调用点写裸名不变），
   `logger.h`/`vtable_helper.h` → `src/utils/`，`include_directories(include)` 删除，工程根不再有 `include/`。
6. ⏳ 收敛三连（各自独立小提交）：`runtime/thread_utils` → `config/paths.h` → `send/broadcast`。
7. ⏳ 高风险拆分留到最后、且必须真机验过一轮再动：`native/closure`、`media/cdn_upload`。


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
* 根据描述文件生成 C++ 的类实现（.pb.cc）和类声明（.pb.h） /Users/user/CLionProjects/sohook/opt/homebrew/bin/protoc --proto_path=proto --cpp_out=proto/gen wework_conversation.proto wework_text_message.proto wework_image_message.proto
*  /opt/homebrew/bin/protoc --proto_path=proto --cpp_out=proto/gen ww_file_message.proto
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
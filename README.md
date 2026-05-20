# SoHook
在 Native 层实现 C++方法的 inline hook，拦截企微发消息方法，并实现 Naive 层发送消息功能。

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

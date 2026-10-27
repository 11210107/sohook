推送发送表情包配置参数到手机

```
adb push /file/emoji.req /storage/emulated/0/Android/data/com.tencent.wework/cache/emoji.req
```

推送链接卡片（图文链接消息）配置参数到手机

```
adb push /file/link.req /storage/emulated/0/Android/data/com.tencent.wework/cache/link.req
```

推送视频消息配置参数到手机（video= 指向的视频要先一并推上去）

```
adb push /file/video.req /storage/emulated/0/Android/data/com.tencent.wework/cache/video.req
```

一次性配置（触发后自动改名 *.sent，避免重复群发；重投前清掉 .sent）：

```
adb shell rm -f /storage/emulated/0/Android/data/com.tencent.wework/cache/{emoji,link,video}.req.sent
```


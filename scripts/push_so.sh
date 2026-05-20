#!/bin/bash

SO_PATH="/data/local/tmp/libsohook.so"
LOCAL_SO="../cmake-build-android-arm64/libsohook.so"

echo "[1/5] 删除旧 SO..."
adb shell "su -c 'rm -f $SO_PATH'"

echo "[2/5] 推送新 SO..."
adb push "$LOCAL_SO" "$SO_PATH"

echo "[3/5] 设置权限..."
adb shell "su -c 'chcon u:object_r:apk_data_file:s0 $SO_PATH'"
adb shell "su -c 'chmod 777 $SO_PATH'"

echo "[4/5] 杀死企业微信..."
adb shell am force-stop com.tencent.wework

sleep 1

echo "[5/5] 启动企业微信..."
adb shell am start \
  -n com.tencent.wework/com.tencent.wework.launch.LaunchSplashActivity

echo "部署完成，企业微信已自动启动"
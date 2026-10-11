#!/bin/bash

# 设备 ID 获取（三级兜底）：
# 1. $ADB_SERIAL：device 函数（~/.zshrc）的本体，export 后脚本可直接继承
# 2. device 命令：若未来改为可执行文件，从其输出解析
# 3. adb devices：取列表中第一台已授权设备
DEVICE_ID="${ADB_SERIAL:-}"
if [ -z "$DEVICE_ID" ] && command -v device >/dev/null 2>&1; then
  DEVICE_ID=$(device | awk -F'Current Device: ' 'NF>1 {print $2}' | tr -d '[:space:]')
fi
if [ -z "$DEVICE_ID" ]; then
  DEVICE_ID=$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')
fi
if [ -z "$DEVICE_ID" ]; then
  echo "未获取到设备 ID，请确认已 export ADB_SERIAL 或设备已连接"
  exit 1
fi
echo "目标设备: $DEVICE_ID"

SO_PATH="/data/local/tmp/libsohook.so"
#LOCAL_SO="../cmake-build-android-arm64/libsohook.so"
LOCAL_SO="../cmake-build-android/libsohook.so"

echo "[1/5] 删除旧 SO..."
adb -s "$DEVICE_ID" shell "su -c 'rm -f $SO_PATH'"

echo "[2/5] 推送新 SO..."
adb -s "$DEVICE_ID" push "$LOCAL_SO" "$SO_PATH"

echo "[3/5] 设置权限..."
adb -s "$DEVICE_ID" shell "su -c 'chcon u:object_r:apk_data_file:s0 $SO_PATH'"
adb -s "$DEVICE_ID" shell "su -c 'chmod 777 $SO_PATH'"

echo "[4/5] 杀死企业微信..."
adb -s "$DEVICE_ID" shell am force-stop com.tencent.wework

sleep 1

echo "[5/5] 启动企业微信..."
adb -s "$DEVICE_ID" shell am start \
  -n com.tencent.wework/com.tencent.wework.launch.LaunchSplashActivity

echo "部署完成，企业微信已自动启动"
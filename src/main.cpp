#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "../include/logger.h"
#include "wework_hook.h"
#include <string>
#include <fstream>

#include <jni.h>

// 获取进程名，确保只在主进程注入，避免干扰小程序等子进程
std::string get_process_name() {
    FILE *fp = fopen("/proc/self/cmdline", "r");
    if (fp) {
        char buf[256] = {0};
        fgets(buf, sizeof(buf), fp);
        fclose(fp);
        return std::string(buf);
    }
    return "unknown";
}

// 在 main 函数外面加这个
__attribute__((constructor))
void my_init() {
    // 既然能走到这里，说明 Zygisk 已经通过 preAppSpecialize 确认了进程
    LOGI("--- SoHook Library Loaded Successfully ---");

    // 直接初始化，不再判断 proc_name == "com.tencent.wework"
    init_wework_hook();
}


/**
 *
 * 下面都是测试代码， ignore
 */
bool std_open(int &value1) {
    LOGI("std_open execute");
    int fd = open("/data/data/com.wz.weworkdb/test.txt",O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (-1 == fd) {
        LOGE("open failed");
        value1 = 1;
        return true;
    }
    char actualpath[PATH_MAX + 1];
    // 尝试通过 fd 获取真实路径（Linux 特有技巧）
    char procfd[64];
    sprintf(procfd, "/proc/self/fd/%d", fd);
    memset(actualpath, 0, sizeof(actualpath));
    readlink(procfd, actualpath, PATH_MAX);

    LOGI("FILE PHYSICALLY CREATED AT: %s", actualpath);
    write(fd, "Hello,Linux\n", 14);
    fsync(fd); // 强制同步到磁盘
    close(fd);
    LOGI("std_open open file test.txt success");
    return false;
}

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.
extern "C" int main(int argc, char **argv) {
    LOGI("Inject main argc:%d", argc);
    // TIP Press <shortcut actionId="RenameElement"/> when your caret is at the <b>lang</b> variable name to see how CLion can help you rename it.
    auto lang = "C++";
    LOGI("Hello and welcome to %s", lang);

    for (int i = 1; i <= 5; i++) {
        // TIP Press <shortcut actionId="Debug"/> to start debugging your code. We have set one <icon src="AllIcons.Debugger.Db_set_breakpoint"/> breakpoint for you, but you can always add more by pressing <shortcut actionId="ToggleLineBreakpoint"/>.
        LOGI("i = %d", i);
    }

    int value1;
    // if (std_open(value1)) return value1;
    // int fd = syscall()


    return 0;
    // TIP See CLion help at <a href="https://www.jetbrains.com/help/clion/">jetbrains.com/help/clion/</a>. Also, you can try interactive lessons for CLion by selecting 'Help | Learn IDE Features' from the main menu.
}

std::string get_package_name() {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string package_name;
    if (cmdline.is_open()) {
        std::getline(cmdline, package_name, '\0');
        cmdline.close();
    }
    return package_name.empty() ? "unknown" : package_name;
}

extern "C" int my_entry(char *argv) {
    LOGI("------------------ SoHook Loaded ------------------");
    std::string pkg = get_package_name();
    LOGI("Target Package:%s", pkg.c_str());
    LOGI("Current PID:%d,UID:%d", getpid(), getuid());


    LOGI("Inject entry point received:%s", argv);

    int val = atoi(argv);
    LOGI("Converted to integer:%d", val);
    int num;
    // 打开文件
    // std_open(num);


    return 0;
}

/**
 * 1. 定义你原本的 C++ 逻辑方法
 * 注意：动态注册的方法签名必须包含 (JNIEnv *, jobject/jclass)
**/
extern "C" jint native_do_hook_logic(JNIEnv *env, jobject thiz, jstring j_argv) {
    LOGI("----------------- SoHook Loaded (Dynamic) -----------------");
    const char *argv = env->GetStringUTFChars(j_argv, nullptr);
    if (argv != nullptr) {
        LOGI("Inject entry point received:%s", argv);
        int val = atoi(argv);
        LOGI("Converted to integer:%d", val);
        env->ReleaseStringUTFChars(j_argv, argv);
    }
    return 0;
}

/**
 * 2.准备映射表
 **/
static JNINativeMethod gMethods[] = {
    // { "Java方法名", "签名(参数类型)返回值类型", (void*)C++函数指针 }
    {"callNativeEntry", "(Ljava/lang/String;)I", (void *) &native_do_hook_logic},
};
/**
 * 3.实现 JNI_OnLoad
 **/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;
    if (vm->GetEnv((void **) &env,JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    // 找到 Java 对应的类名（注意包名分隔符是/）
    jclass clazz = env->FindClass("com/wz/weworkdb/NativeBridge");
    if (clazz == nullptr) {
        LOGE("FindClass failed:com/wz/weworkdb/NativeBridge");
        return JNI_ERR;
    }
    // 注册方法
    if (env->RegisterNatives(clazz, gMethods, sizeof(gMethods) / sizeof(gMethods[0])) < 0) {
        LOGE("RegisterNatives failed");
        return JNI_ERR;
    }
    LOGI("JNI_OnLoad:Dynamic registration successful");
    return JNI_VERSION_1_6;
}

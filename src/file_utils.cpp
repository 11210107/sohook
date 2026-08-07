//
// Created by user_wangzhen on 2026/5/20.
//
#include "file_utils.h"
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "logger.h"
// 从指定路径读取二进制文件，返回字节 vector
std::vector<uint8_t> read_binary_file(const std::string& file_path) {
    // 💡 以二进制模式并直接定位到文件末尾（ios::ate）来打开文件，方便直接获取大小
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);

    if (!file.is_open()) {
        LOGE("[-] 错误: 无法打开文件 %s", file_path.c_str());
        return {};
    }

    // 获取文件大小
    std::streamsize size = file.tellg();
    // 重新将文件指针移回开头
    file.seekg(0, std::ios::beg);

    // 分配对应的内存空间
    std::vector<uint8_t> buffer(size);

    // 一次性读取整块数据
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        LOGI("[+] 成功读取二进制文件: %s, 大小: %lld 字节", file_path.c_str(), (long long)size);
        return buffer;
    } else {
        LOGE("[-] 错误: 读取文件数据失败 %s", file_path.c_str());
        return {};
    }
}


static std::string jstring_to_std(JNIEnv *env, jstring js) {
    if (!js) return "";
    const char *c = env->GetStringUTFChars(js, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    return s;
}

// 取出当前挂起的 Java 异常描述（类名+消息），并清除异常状态
static std::string jni_exception_to_std(JNIEnv *env) {
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    if (!ex) return "";
    jclass exCls = env->GetObjectClass(ex);
    jmethodID toStr = env->GetMethodID(exCls, "toString", "()Ljava/lang/String;");
    jstring jMsg = (jstring) env->CallObjectMethod(ex, toStr);
    std::string msg = jstring_to_std(env, jMsg);
    env->DeleteLocalRef(jMsg);
    env->DeleteLocalRef(exCls);
    env->DeleteLocalRef(ex);
    return msg;
}

std::string resolve_content_uri_to_local(JNIEnv *env,const std::string &uri_str) {
    if (!env) {
        LOGE("resolve_uri: no jni env");
        return "";
    }

    jclass atCls = nullptr, ctxCls = nullptr, uriCls = nullptr, crCls = nullptr;
    jclass strCls = nullptr, curCls = nullptr, fileCls = nullptr, pfdCls = nullptr;
    jobject appCtx = nullptr, resolver = nullptr, uri = nullptr, pfd = nullptr;
    jobject cursor = nullptr, filesDir = nullptr;
    jstring jUriStr = nullptr, jMode = nullptr;
    jobjectArray proj = nullptr;
    int fd = -1, out = -1;

    // 提前声明清理 lambda，确保任何分支返回前释放 local ref 与 fd
    auto cleanup = [&]() {
        if (out >= 0) close(out);
        if (fd >= 0) close(fd);
        if (jUriStr) env->DeleteLocalRef(jUriStr);
        if (jMode) env->DeleteLocalRef(jMode);
        if (proj) env->DeleteLocalRef(proj);
        if (cursor) env->DeleteLocalRef(cursor);
        if (filesDir) env->DeleteLocalRef(filesDir);
        if (pfd) env->DeleteLocalRef(pfd);
        if (uri) env->DeleteLocalRef(uri);
        if (resolver) env->DeleteLocalRef(resolver);
        if (appCtx) env->DeleteLocalRef(appCtx);
        if (atCls) env->DeleteLocalRef(atCls);
        if (ctxCls) env->DeleteLocalRef(ctxCls);
        if (uriCls) env->DeleteLocalRef(uriCls);
        if (crCls) env->DeleteLocalRef(crCls);
        if (strCls) env->DeleteLocalRef(strCls);
        if (curCls) env->DeleteLocalRef(curCls);
        if (fileCls) env->DeleteLocalRef(fileCls);
        if (pfdCls) env->DeleteLocalRef(pfdCls);
    };

    // 1. Application Context
    atCls = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(atCls, "currentApplication", "()Landroid/app/Application;");
    appCtx = env->CallStaticObjectMethod(atCls, curApp);

    // 2. ContentResolver
    ctxCls = env->FindClass("android/content/Context");
    jmethodID getCr = env->GetMethodID(ctxCls, "getContentResolver", "()Landroid/content/ContentResolver;");
    resolver = env->CallObjectMethod(appCtx, getCr);

    // 3. Uri.parse
    uriCls = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jUriStr = env->NewStringUTF(uri_str.c_str());
    uri = env->CallStaticObjectMethod(uriCls, parse, jUriStr);

    // 4. openFileDescriptor（可能抛 SecurityException：未授权或 URI 无效）
    crCls = env->FindClass("android/content/ContentResolver");
    jmethodID openFd = env->GetMethodID(crCls, "openFileDescriptor",
                                        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jMode = env->NewStringUTF("r");
    pfd = env->CallObjectMethod(resolver, openFd, uri, jMode);
    if (env->ExceptionCheck()) {
        std::string ex = jni_exception_to_std(env);
        LOGE("resolve_uri: openFileDescriptor 异常: %s", ex.c_str());
        cleanup();
        return "";
    }
    if (!pfd) {
        LOGE("resolve_uri: ParcelFileDescriptor 为空");
        cleanup();
        return "";
    }

    // 5. 查询 DISPLAY_NAME 作为目标文件名
    std::string display_name;
    jmethodID query = env->GetMethodID(crCls, "query",
                                       "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
    strCls = env->FindClass("java/lang/String");
    proj = env->NewObjectArray(1, strCls, env->NewStringUTF("_display_name"));
    cursor = env->CallObjectMethod(resolver, query, uri, proj, nullptr, nullptr, nullptr);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        cursor = nullptr;
    }
    if (cursor) {
        curCls = env->FindClass("android/database/Cursor");
        jmethodID moveFirst = env->GetMethodID(curCls, "moveToFirst", "()Z");
        jmethodID getStr = env->GetMethodID(curCls, "getString", "(I)Ljava/lang/String;");
        jmethodID closeCur = env->GetMethodID(curCls, "close", "()V");
        if (env->CallBooleanMethod(cursor, moveFirst)) {
            jstring jName = (jstring) env->CallObjectMethod(cursor, getStr, 0);
            display_name = jstring_to_std(env, jName);
            env->DeleteLocalRef(jName);
        }
        env->CallVoidMethod(cursor, closeCur);
    }
    if (display_name.empty()) {
        display_name = "wxsdk_" + std::to_string(time(nullptr));
    }

    // 6. 目标路径：企微私有 files 目录
    jmethodID getFilesDir = env->GetMethodID(ctxCls, "getFilesDir", "()Ljava/io/File;");
    filesDir = env->CallObjectMethod(appCtx, getFilesDir);
    fileCls = env->FindClass("java/io/File");
    jmethodID getAbs = env->GetMethodID(fileCls, "getAbsolutePath", "()Ljava/lang/String;");
    std::string base_dir = jstring_to_std(env, (jstring) env->CallObjectMethod(filesDir, getAbs));

    // 7. detachFd 得到原生 fd
    pfdCls = env->FindClass("android/os/ParcelFileDescriptor");
    jmethodID detachFd = env->GetMethodID(pfdCls, "detachFd", "()I");
    fd = env->CallIntMethod(pfd, detachFd);
    if (fd < 0) {
        LOGE("resolve_uri: detachFd 失败");
        cleanup();
        return "";
    }

    // 落到企微文件消息目录结构 files/share/<毫秒时间戳>/<文件名>
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string share_dir = base_dir + "/share";
    mkdir(share_dir.c_str(), 0755);
    std::string dest_dir = share_dir + "/" + std::to_string(ms);
    mkdir(dest_dir.c_str(), 0755);
    std::string out_path = dest_dir + "/" + display_name;

    out = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        LOGE("resolve_uri: 目标文件打开失败: %s", out_path.c_str());
        cleanup();
        return "";
    }

    // 8. fd 内容复制到本地文件
    char buf[8192];
    ssize_t n;
    bool ok = true;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out, buf + written, n - written);
            if (w < 0) {
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }

    cleanup();

    if (!ok) {
        LOGE("resolve_uri: 写入失败: %s", out_path.c_str());
        return "";
    }
    LOGI("resolve_uri: %s -> %s", uri_str.c_str(), out_path.c_str());
    return out_path;
}

// 获取文件大小（字节），失败返回 0
uint64_t get_file_size(const std::string& file_path) {
    struct stat stat_buf;
    if (stat(file_path.c_str(), &stat_buf) == 0) {
        return static_cast<uint64_t>(stat_buf.st_size);
    }
    LOGE("[-] 无法获取文件大小: %s", file_path.c_str());
    return 0;
}

// 💡 辅助函数：读取 2 个字节的大端序整型
uint16_t read_be_uint16(const unsigned char* buffer) {
    return (static_cast<uint16_t>(buffer[0]) << 8) | static_cast<uint16_t>(buffer[1]);
}

uint32_t read_be_uint32(const unsigned char* buffer) {
    return (static_cast<uint32_t>(buffer[0]) << 24) |
           (static_cast<uint32_t>(buffer[1]) << 16) |
           (static_cast<uint32_t>(buffer[2]) << 8)  |
           static_cast<uint32_t>(buffer[3]);
}

/**
 * 🎯 动态解析本地图片（智能支持 PNG 和 JPG）
 */
bool get_image_info(const std::string& path, uint64_t& out_size, uint32_t& out_width, uint32_t& out_height) {
    // 1. 获取文件物理大小
    struct stat stat_buf;
    if (stat(path.c_str(), &stat_buf) == 0) {
        out_size = stat_buf.st_size;
    } else {
        LOGE("[-] 无法获取文件大小: %s", path.c_str());
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("[-] 无法打开文件: %s", path.c_str());
        return false;
    }

    // 2. 读取前 4 个字节判断文件类型
    unsigned char magic[4];
    file.read(reinterpret_cast<char*>(magic), 4);
    if (file.gcount() < 4) return false;

    // ---- A 流程：如果是 PNG ----
    if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
        unsigned char ihdr[20]; // 接着读完 IHDR 块
        file.read(reinterpret_cast<char*>(ihdr), 20);
        if (file.gcount() < 20) return false;

        // PNG 的宽和高在特定偏移
        out_width = read_be_uint32(&ihdr[12]);
        out_height = read_be_uint32(&ihdr[16]);
        LOGI("[+] 解析成功 [PNG]: 宽=%u, 高=%u, 大小=%lu", out_width, out_height, out_size);
        return true;
    }

    // ---- B 流程：如果是 JPG ----
    // JPG 固定以 0xFF 0xD8 开头
    if (magic[0] == 0xFF && magic[1] == 0xD8) {
        // 回到文件开头的第 2 字节，开始挨个段扫描
        file.seekg(2, std::ios::beg);

        unsigned char marker[2];
        while (file.read(reinterpret_cast<char*>(marker), 2)) {
            // 所有的有效标记位都必须以 0xFF 开头
            if (marker[0] != 0xFF) break;

            // 💡 核心：0xC0 或 0xC2 代表 SOF (Start of Frame) 段，里面存着图像宽高
            if (marker[1] == 0xC0 || marker[1] == 0xC1 || marker[1] == 0xC2 || marker[1] == 0xC3) {
                // 跳过 2 字节的段长度和 1 字节的精度 (共 3 字节)
                file.seekg(3, std::ios::cur);

                unsigned char size_buf[4];
                file.read(reinterpret_cast<char*>(size_buf), 4);

                // ⚠️ 注意：JPG 的存储顺序是【先高后宽】，都是 2 字节短整型
                out_height = read_be_uint16(&size_buf[0]);
                out_width  = read_be_uint16(&size_buf[2]);

                LOGI("[+] 解析成功 [JPG]: 宽=%u, 高=%u, 大小=%lu", out_width, out_height, out_size);
                return true;
            } else {
                // 如果是别的没用的数据段（比如 APP0, COM 等），直接跳过它
                unsigned char len_buf[2];
                if (!file.read(reinterpret_cast<char*>(len_buf), 2)) break;
                uint16_t chunk_len = read_be_uint16(len_buf);
                // 减去长度自身占用的 2 字节，向后跳跃
                file.seekg(chunk_len - 2, std::ios::cur);
            }
        }
    }

    LOGE("[-] 未知或不支持的图片格式: %s", path.c_str());
    return false;
}
//
// Created by user_wangzhen on 2026/5/20.
//
#include "file_utils.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
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


// 辅助函数：递归创建多级目录 (类似 mkdir -p)
bool mkdir_p(const std::string& path) {
    size_t pos = 0;
    do {
        pos = path.find_first_of('/', pos + 1);
        std::string sub_path = path.substr(0, pos);
        if (!sub_path.empty()) {
            if (mkdir(sub_path.c_str(), 0777) && errno != EEXIST) {
                LOGE("mkdir_p 失败: %s, error: %s", sub_path.c_str(), strerror(errno));
                return false;
            }
        }
    } while (pos != std::string::npos);
    return true;
}

std::string prepare_sandbox_file(const std::string& src_path, uint64_t conv_id) {
    // 1. 检查源文件是否存在且可读
    std::ifstream src(src_path, std::ios::binary);
    if (!src.is_open()) {
        LOGE("无法读取源文件 (可能缺少存储权限): %s", src_path.c_str());
        return "";
    }

    // 2. 提取文件名
    size_t last_slash = src_path.find_last_of("/\\");
    std::string file_name = (last_slash == std::string::npos) ? src_path : src_path.substr(last_slash + 1);

    // 3. 构建目标目录并递归创建
    std::string sandbox_dir = "/data/user/0/com.tencent.wework/files/share/" + std::to_string(conv_id) + "/";
    if (!mkdir_p(sandbox_dir)) {
        LOGE("创建沙箱目录失败: %s", sandbox_dir.c_str());
        return "";
    }

    // 4. 复制文件
    std::string dest_path = sandbox_dir + file_name;
    std::ofstream dst(dest_path, std::ios::binary);
    if (!dst.is_open()) {
        LOGE("无法写入目标沙箱文件: %s, error: %s", dest_path.c_str(), strerror(errno));
        return "";
    }

    dst << src.rdbuf();
    LOGI("文件成功复制到沙箱: %s -> %s", src_path.c_str(), dest_path.c_str());
    return dest_path;
}

// 根据扩展名推断 MIME 类型
static std::string guess_mime_type(const std::string& file_name) {
    size_t dot = file_name.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = file_name.substr(dot);
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png")  return "image/png";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".txt")  return "text/plain";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".doc" || ext == ".docx") return "application/msword";
    if (ext == ".xls" || ext == ".xlsx") return "application/vnd.ms-excel";
    if (ext == ".zip")  return "application/zip";
    if (ext == ".apk")  return "application/vnd.android.package-archive";
    return "application/octet-stream";
}

// 通过 ContentResolver + MediaStore 查询获取 content URI，再打开 InputStream
// 若查询不到则通过 insert 将文件注册到 MediaStore
// 解决 Android 10+ Scoped Storage 限制，替代直接 FileInputStream 路径访问
static jobject open_input_stream_via_mediastore(JNIEnv* env, jobject context, const std::string& src_path) {
    // 1. 解析路径：提取文件名
    size_t last_slash = src_path.find_last_of("/\\");
    std::string file_name = (last_slash == std::string::npos) ? src_path : src_path.substr(last_slash + 1);

    // 2. 获取 ContentResolver
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getContentResolver = env->GetMethodID(contextClass, "getContentResolver",
                                                     "()Landroid/content/ContentResolver;");
    jobject resolver = env->CallObjectMethod(context, getContentResolver);

    // 3. 获取 MediaStore.Files 相关类
    jclass mediaStoreFilesClass = env->FindClass("android/provider/MediaStore$Files");
    jclass mediaStoreColumnsClass = env->FindClass("android/provider/MediaStore$Files$FileColumns");

    jfieldID idField = env->GetStaticFieldID(mediaStoreColumnsClass, "_ID", "Ljava/lang/String;");
    jfieldID dataField = env->GetStaticFieldID(mediaStoreColumnsClass, "DATA", "Ljava/lang/String;");
    jfieldID displayNameField = env->GetStaticFieldID(mediaStoreColumnsClass, "DISPLAY_NAME", "Ljava/lang/String;");

    jstring idColumn = (jstring) env->GetStaticObjectField(mediaStoreColumnsClass, idField);
    jstring dataColumn = (jstring) env->GetStaticObjectField(mediaStoreColumnsClass, dataField);
    jstring displayNameColumn = (jstring) env->GetStaticObjectField(mediaStoreColumnsClass, displayNameField);

    // 4. 获取 MediaStore.Files.getContentUri("external")
    jmethodID getContentUri = env->GetStaticMethodID(
        mediaStoreFilesClass, "getContentUri", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring externalStr = env->NewStringUTF("external");
    jobject filesUri = env->CallStaticObjectMethod(mediaStoreFilesClass, getContentUri, externalStr);

    jclass resolverClass = env->FindClass("android/content/ContentResolver");
    jmethodID openInputStream = env->GetMethodID(
        resolverClass, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;");

    jobject contentUri = nullptr;

    // ──── 策略 A：先尝试查询 MediaStore ────
    {
        jobjectArray projection = env->NewObjectArray(2, env->FindClass("java/lang/String"), nullptr);
        env->SetObjectArrayElement(projection, 0, idColumn);
        env->SetObjectArrayElement(projection, 1, dataColumn);

        jstring displayNameValue = env->NewStringUTF(file_name.c_str());
        jobjectArray selectionArgs = env->NewObjectArray(1, env->FindClass("java/lang/String"), nullptr);
        env->SetObjectArrayElement(selectionArgs, 0, displayNameValue);

        jmethodID queryMethod = env->GetMethodID(resolverClass, "query",
            "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)"
            "Landroid/database/Cursor;");
        jobject cursor = env->CallObjectMethod(resolver, queryMethod, filesUri, projection,
                                                displayNameColumn, selectionArgs, nullptr);

        if (cursor != nullptr) {
            jclass cursorClass = env->FindClass("android/database/Cursor");
            jmethodID moveToFirst = env->GetMethodID(cursorClass, "moveToFirst", "()Z");
            jmethodID moveToNext = env->GetMethodID(cursorClass, "moveToNext", "()Z");
            jmethodID getColumnIndex = env->GetMethodID(cursorClass, "getColumnIndex", "(Ljava/lang/String;)I");
            jmethodID getString = env->GetMethodID(cursorClass, "getString", "(I)Ljava/lang/String;");
            jmethodID getLong = env->GetMethodID(cursorClass, "getLong", "(I)J");
            jmethodID closeCursor = env->GetMethodID(cursorClass, "close", "()V");

            jint dataIdx = env->CallIntMethod(cursor, getColumnIndex, dataColumn);
            jint idIdx = env->CallIntMethod(cursor, getColumnIndex, idColumn);

            jlong fileId = -1;
            if (env->CallBooleanMethod(cursor, moveToFirst)) {
                do {
                    jstring cursorDataPath = (jstring) env->CallObjectMethod(cursor, getString, dataIdx);
                    if (cursorDataPath != nullptr) {
                        const char* dataPath = env->GetStringUTFChars(cursorDataPath, nullptr);
                        if (dataPath != nullptr && src_path == dataPath) {
                            fileId = env->CallLongMethod(cursor, getLong, idIdx);
                            env->ReleaseStringUTFChars(cursorDataPath, dataPath);
                            break;
                        }
                        env->ReleaseStringUTFChars(cursorDataPath, dataPath);
                    }
                } while (env->CallBooleanMethod(cursor, moveToNext));
            }
            env->CallVoidMethod(cursor, closeCursor);

            if (fileId != -1) {
                jclass contentUrisClass = env->FindClass("android/content/ContentUris");
                jmethodID withAppendedId = env->GetStaticMethodID(
                    contentUrisClass, "withAppendedId", "(Landroid/net/Uri;J)Landroid/net/Uri;");
                contentUri = env->CallStaticObjectMethod(contentUrisClass, withAppendedId, filesUri, fileId);
                LOGI("[+] MediaStore 查询命中: %s", src_path.c_str());
            }
        }
    }

    // ──── 策略 B：查询未命中，通过 insert 将文件注册到 MediaStore ────
    if (contentUri == nullptr) {
        LOGI("[*] MediaStore 查询未命中，尝试 insert 注册文件: %s", src_path.c_str());

        jclass contentValuesClass = env->FindClass("android/content/ContentValues");
        jmethodID cvConstructor = env->GetMethodID(contentValuesClass, "<init>", "()V");
        jobject cv = env->NewObject(contentValuesClass, cvConstructor);

        jmethodID putString = env->GetMethodID(contentValuesClass, "put",
                                                "(Ljava/lang/String;Ljava/lang/String;)V");
        jmethodID putLong = env->GetMethodID(contentValuesClass, "put",
                                              "(Ljava/lang/String;Ljava/lang/Long;)V");

        // DISPLAY_NAME
        jstring keyDisp = env->NewStringUTF("DISPLAY_NAME");
        jstring valDisp = env->NewStringUTF(file_name.c_str());
        env->CallVoidMethod(cv, putString, keyDisp, valDisp);

        // DATA (deprecated 但 Android 10 仍可用)
        jstring keyData = env->NewStringUTF("DATA");
        jstring valData = env->NewStringUTF(src_path.c_str());
        env->CallVoidMethod(cv, putString, keyData, valData);

        // MIME_TYPE
        std::string mime = guess_mime_type(file_name);
        jstring keyMime = env->NewStringUTF("MIME_TYPE");
        jstring valMime = env->NewStringUTF(mime.c_str());
        env->CallVoidMethod(cv, putString, keyMime, valMime);

        // IS_PENDING = 0 (文件已存在，无需 pending)
        jstring keyPending = env->NewStringUTF("IS_PENDING");
        jclass longClass = env->FindClass("java/lang/Long");
        jmethodID longValueOf = env->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
        jobject valZero = env->CallStaticObjectMethod(longClass, longValueOf, (jlong)0);
        env->CallVoidMethod(cv, putLong, keyPending, valZero);

        jmethodID insertMethod = env->GetMethodID(resolverClass, "insert",
                                                   "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
        contentUri = env->CallObjectMethod(resolver, insertMethod, filesUri, cv);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("[-] MediaStore insert 异常: %s", src_path.c_str());
            contentUri = nullptr;
        } else if (contentUri != nullptr) {
            LOGI("[+] MediaStore insert 注册成功: %s", src_path.c_str());
        }
    }

    // ──── 策略 C：使用 contentUri 打开 InputStream ────
    if (contentUri == nullptr) {
        LOGE("[-] 无法获取 content URI: %s", src_path.c_str());
        return nullptr;
    }

    jobject inputStream = env->CallObjectMethod(resolver, openInputStream, contentUri);

    if (env->ExceptionCheck() || inputStream == nullptr) {
        env->ExceptionClear();
        LOGE("[-] ContentResolver.openInputStream 失败: %s", src_path.c_str());
        return nullptr;
    }

    LOGI("[+] 通过 MediaStore 成功打开文件: %s", src_path.c_str());
    return inputStream;
}

// 通过 JNI 获取 App 私有沙盒目录，并将源文件复制到其中
// 使用 ContentResolver + MediaStore 规避 Android 10+ Scoped Storage 限制
std::string copy_to_sandbox_via_jni(JNIEnv* env, const std::string& src_path) {
    // 1. 获取 Application 上下文
    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    if (activityThreadClass == nullptr) {
        LOGE("[-] FindClass ActivityThread 失败");
        return "";
    }

    jmethodID currentActivityThread = env->GetStaticMethodID(
        activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (currentActivityThread == nullptr) {
        LOGE("[-] 获取 currentActivityThread 方法失败");
        return "";
    }
    jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThread);
    if (activityThread == nullptr) {
        LOGE("[-] 获取 ActivityThread 实例失败");
        return "";
    }

    jmethodID getApplication = env->GetMethodID(
        activityThreadClass, "getApplication", "()Landroid/app/Application;");
    if (getApplication == nullptr) {
        LOGE("[-] 获取 getApplication 方法失败");
        return "";
    }
    jobject application = env->CallObjectMethod(activityThread, getApplication);
    if (application == nullptr) {
        LOGE("[-] 获取 Application 实例失败");
        return "";
    }

    // 2. 通过 MediaStore 获取源文件 InputStream
    jobject inputStream = open_input_stream_via_mediastore(env, application, src_path);
    if (inputStream == nullptr) {
        LOGE("[-] 无法通过 MediaStore 打开源文件: %s", src_path.c_str());
        return "";
    }

    // 3. 获取沙盒 files 目录
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getFilesDir = env->GetMethodID(contextClass, "getFilesDir", "()Ljava/io/File;");
    if (getFilesDir == nullptr) {
        LOGE("[-] 获取 getFilesDir 方法失败");
        return "";
    }
    jobject filesDir = env->CallObjectMethod(application, getFilesDir);
    if (filesDir == nullptr) {
        LOGE("[-] 获取 filesDir 失败");
        return "";
    }

    jclass fileClass = env->FindClass("java/io/File");
    jmethodID getAbsolutePath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    jstring dirPath = (jstring) env->CallObjectMethod(filesDir, getAbsolutePath);

    const char* sandbox_dir = env->GetStringUTFChars(dirPath, nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string dest_dir = std::string(sandbox_dir) + "/share/" + std::to_string(ms) + "/";
    env->ReleaseStringUTFChars(dirPath, sandbox_dir);

    LOGI("[+] 获取沙盒目录成功: %s", dest_dir.c_str());

    // 4. 创建目标子目录
    if (!mkdir_p(dest_dir)) {
        LOGE("[-] 创建沙盒子目录失败: %s", dest_dir.c_str());
        return "";
    }

    // 5. 提取文件名并构建目标路径
    size_t last_slash = src_path.find_last_of("/\\");
    std::string file_name = (last_slash == std::string::npos) ? src_path : src_path.substr(last_slash + 1);
    std::string dest_path = dest_dir + file_name;

    // 6. 通过 JNI FileOutputStream 写入目标文件
    jclass fosClass = env->FindClass("java/io/FileOutputStream");
    if (fosClass == nullptr) {
        LOGE("[-] FindClass FileOutputStream 失败");
        return "";
    }
    jmethodID fosConstructor = env->GetMethodID(fosClass, "<init>", "(Ljava/lang/String;)V");
    jstring destPathStr = env->NewStringUTF(dest_path.c_str());
    jobject fos = env->NewObject(fosClass, fosConstructor, destPathStr);
    if (env->ExceptionCheck() || fos == nullptr) {
        env->ExceptionClear();
        LOGE("[-] 无法创建目标文件 FileOutputStream: %s", dest_path.c_str());
        return "";
    }

    // 7. 通过 JNI 逐块拷贝（8KB 缓冲区）
    jbyteArray buffer = env->NewByteArray(8192);
    jclass isClass = env->FindClass("java/io/InputStream");
    jmethodID readMethod = env->GetMethodID(isClass, "read", "([B)I");
    jmethodID writeMethod = env->GetMethodID(fosClass, "write", "([BII)V");

    jint bytesRead;
    int64_t totalBytes = 0;
    while ((bytesRead = env->CallIntMethod(inputStream, readMethod, buffer)) > 0) {
        env->CallVoidMethod(fos, writeMethod, buffer, 0, bytesRead);
        totalBytes += bytesRead;
    }

    // 8. 关闭流
    jmethodID closeIS = env->GetMethodID(isClass, "close", "()V");
    jmethodID closeOS = env->GetMethodID(fosClass, "close", "()V");
    env->CallVoidMethod(inputStream, closeIS);
    env->CallVoidMethod(fos, closeOS);

    LOGI("[+] 文件成功复制到沙盒: %s -> %s (共 %lld 字节)", src_path.c_str(), dest_path.c_str(), (long long)totalBytes);
    return dest_path;
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
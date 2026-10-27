//
// Created by user_wangzhen on 2026/5/20.
//
#include "utils/file_utils.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>
#include <sys/stat.h>
#include "utils/logger.h"
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

// 文本文件全文（req 配置读取共用入口；不存在/为空即返回空串，由调用方判空）
std::string read_text_file(const std::string &file_path) {
    const std::vector<uint8_t> bytes = read_binary_file(file_path);
    return std::string(bytes.begin(), bytes.end());
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

uint64_t read_be_uint64(const unsigned char* buffer) {
    return (static_cast<uint64_t>(read_be_uint32(buffer)) << 32) | read_be_uint32(buffer + 4);
}

// ===== MP4 视频元数据解析（moov/mvhd/tkhd box 级；封面生成不在本模块）=====
// 零第三方依赖：顶层 box seek-walk（moov 在头/在尾都覆盖，mdat 等大 box 只跳不读）
// + moov 全量载入内存解析。宽高/时长仅取 moov 元数据，不解码媒体数据。

// 在 buf[pos] 读一个子 box 头，输出 type/payload 偏移/payload 长度；越界/非法返回 false
static bool mp4_next_box(const uint8_t *buf, size_t len, size_t pos,
                         const uint8_t *&type, size_t &payload, size_t &plen) {
    if (pos + 8 > len) return false;
    uint64_t size = read_be_uint32(buf + pos);
    type = buf + pos + 4;
    uint64_t hdr = 8;
    if (size == 1) {          // largesize：64 位尺寸跟在头部
        if (pos + 16 > len) return false;
        size = read_be_uint64(buf + pos + 8);
        hdr = 16;
    } else if (size == 0) {   // size==0：本 box 延伸到 buffer 末尾
        size = len - pos;
    }
    if (size < hdr || size > len - pos) return false;
    payload = pos + hdr;
    plen = static_cast<size_t>(size - hdr);
    return true;
}

static bool mp4_is_type(const uint8_t *type, const char *fourcc) {
    return memcmp(type, fourcc, 4) == 0;
}

// 顶层 box walk：seek 跳过非 moov box（mdat 可为 GB 级，绝不读入）；
// 找到 moov 后全量读入（相机/导出文件的 moov 为 KB 级；64MB 防护上限）
static bool mp4_load_moov(FILE *f, uint64_t file_size, std::vector<uint8_t> &moov) {
    uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        if (fseeko(f, static_cast<off_t>(pos), SEEK_SET) != 0) return false;
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) return false;
        uint64_t size = read_be_uint32(hdr);
        const uint8_t *type = hdr + 4;
        uint64_t hdr_len = 8;
        if (size == 1) {
            unsigned char ls[8];
            if (fread(ls, 1, 8, f) != 8) return false;
            size = read_be_uint64(ls);
            hdr_len = 16;
        } else if (size == 0) {
            size = file_size - pos;
        }
        if (size < hdr_len || size > file_size - pos) return false;
        if (mp4_is_type(type, "moov")) {
            const size_t plen = static_cast<size_t>(size - hdr_len);
            if (plen > 64u * 1024 * 1024) {
                LOGE("[-] moov box 异常巨大(%zu 字节)，拒绝载入", plen);
                return false;
            }
            moov.resize(plen);
            if (fseeko(f, static_cast<off_t>(pos + hdr_len), SEEK_SET) != 0) return false;
            if (fread(moov.data(), 1, plen, f) != plen) return false;
            return true;
        }
        pos += size;
    }
    return false;
}

// mvhd：v0 payload 偏移 12/16 为 timescale/duration；v1 为 20/24（8 字节时长）
static void mp4_parse_mvhd(const uint8_t *p, size_t n, uint64_t &timescale, uint64_t &duration) {
    if (n < 4) return;
    if (p[0] == 1) {
        if (n >= 32) {
            timescale = read_be_uint32(p + 20);
            duration = read_be_uint64(p + 24);
        }
    } else if (n >= 20) {
        timescale = read_be_uint32(p + 12);
        duration = read_be_uint32(p + 16);
    }
}

// tkhd：宽高为 16.16 定点（取高 16 位）；matrix 为 9×int32(16.16)，
// m[1]/m[2] 非 0 = 90°/270°旋转 → 显示宽高互换（与 MediaMetadataRetriever 语义一致）
static bool mp4_parse_tkhd(const uint8_t *p, size_t n, uint32_t &w, uint32_t &h, bool &rotated) {
    if (n < 4) return false;
    size_t off_matrix, off_wh;
    if (p[0] == 1) {
        if (n < 96) return false;
        off_matrix = 52;
        off_wh = 88;
    } else {
        if (n < 84) return false;
        off_matrix = 40;
        off_wh = 76;
    }
    const int32_t m1 = static_cast<int32_t>(read_be_uint32(p + off_matrix + 4));  // m[1]
    const int32_t m2 = static_cast<int32_t>(read_be_uint32(p + off_matrix + 8));  // m[2]
    rotated = (m1 != 0 || m2 != 0);
    w = read_be_uint32(p + off_wh) >> 16;
    h = read_be_uint32(p + off_wh + 4) >> 16;
    if (rotated) std::swap(w, h);
    return true;
}

/**
 * 🎯 解析本地 MP4 视频：大小/宽高/时长（视频消息 content PB 的 4 个本地元数据源）
 *  - 供 build_video_content_pb 的 size(3)/video_duration(4)/高(5)宽(6)取值；
 *  - duration_ms → 秒：真机样例 duration=3 无法区分截断/四舍五入，调用方自行决定
 *    （建议 (ms + 500) / 1000 四舍五入）；
 *  - 选轨：优先 hdlr=='vide' 且宽高非零；否则首个宽高非零 trak（音轨 tkhd 为 0x0）。
 */
bool get_video_info(const std::string &path, uint64_t &out_size, uint32_t &out_width,
                    uint32_t &out_height, uint32_t &out_duration_ms) {
    // 1. 文件物理大小
    struct stat stat_buf;
    if (stat(path.c_str(), &stat_buf) != 0) {
        LOGE("[-] 无法获取视频文件大小: %s", path.c_str());
        return false;
    }
    out_size = static_cast<uint64_t>(stat_buf.st_size);

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        LOGE("[-] 无法打开视频文件: %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> moov;
    const bool loaded = mp4_load_moov(f, out_size, moov);
    fclose(f);
    if (!loaded) {
        LOGE("[-] 未找到 moov box（非 MP4 或文件损坏）: %s", path.c_str());
        return false;
    }

    // 2. 遍历 moov 子 box：mvhd 取时长；trak 逐个取 tkhd 宽高 + mdia/hdlr 判轨型
    uint64_t timescale = 0, duration = 0;
    uint32_t best_w = 0, best_h = 0;
    bool found_video_track = false;
    size_t pos = 0;
    while (true) {
        const uint8_t *type;
        size_t payload, plen;
        if (!mp4_next_box(moov.data(), moov.size(), pos, type, payload, plen)) break;
        if (mp4_is_type(type, "mvhd")) {
            mp4_parse_mvhd(moov.data() + payload, plen, timescale, duration);
        } else if (mp4_is_type(type, "trak")) {
            // trak 内部：tkhd 宽高 + mdia/hdlr 的 handler 4CC
            uint32_t tk_w = 0, tk_h = 0;
            bool tk_rotated = false, is_vide = false, has_tkhd = false;
            size_t sub = 0;
            while (true) {
                const uint8_t *stype;
                size_t spayload, splen;
                if (!mp4_next_box(moov.data(), moov.size(), payload + sub, stype, spayload, splen)) break;
                if (mp4_is_type(stype, "tkhd")) {
                    has_tkhd = mp4_parse_tkhd(moov.data() + spayload, splen, tk_w, tk_h, tk_rotated);
                } else if (mp4_is_type(stype, "mdia")) {
                    size_t md = 0;
                    while (true) {
                        const uint8_t *mtype;
                        size_t mpayload, mplen;
                        if (!mp4_next_box(moov.data(), moov.size(), spayload + md, mtype, mpayload, mplen)) break;
                        if (mp4_is_type(mtype, "hdlr") && mplen >= 12 &&
                            mp4_is_type(moov.data() + mpayload + 8, "vide")) {
                            is_vide = true;
                        }
                        md = (mpayload + mplen) - spayload;
                    }
                }
                sub = (spayload + splen) - payload;
            }
            // 选轨：vide 轨直接命中；否则记首个宽高非零轨作 fallback
            if (has_tkhd && tk_w > 0 && tk_h > 0) {
                if (is_vide) {
                    best_w = tk_w;
                    best_h = tk_h;
                    found_video_track = true;
                } else if (best_w == 0 && !found_video_track) {
                    best_w = tk_w;
                    best_h = tk_h;
                }
            }
        }
        pos = payload + plen;
    }

    if (best_w == 0 || best_h == 0) {
        LOGE("[-] moov 中未找到有效视频轨（tkhd 宽高全零）: %s", path.c_str());
        return false;
    }

    out_width = best_w;
    out_height = best_h;
    out_duration_ms = (timescale > 0)
        ? static_cast<uint32_t>(duration * 1000 / timescale)
        : 0;
    LOGI("[+] 解析成功 [MP4]: 宽=%u, 高=%u, 时长=%ums, 大小=%llu, 路径=%s",
         out_width, out_height, out_duration_ms,
         static_cast<unsigned long long>(out_size), path.c_str());
    return true;
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

// ===== 图片头解析（PNG/JPEG/GIF/BMP；工程 get_image_info 无 GIF，本模块自带）=====
bool parse_image_info(const std::vector<uint8_t> &data, uint32_t &w, uint32_t &h) {
    if (data.size() < 12) return false;
    // PNG：89 50 4E 47，IHDR 宽高 BE @16/@20
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        if (data.size() < 24) return false;
        w = (static_cast<uint32_t>(data[16]) << 24) | (static_cast<uint32_t>(data[17]) << 16) |
            (static_cast<uint32_t>(data[18]) << 8) | data[19];
        h = (static_cast<uint32_t>(data[20]) << 24) | (static_cast<uint32_t>(data[21]) << 16) |
            (static_cast<uint32_t>(data[22]) << 8) | data[23];
        return w > 0 && h > 0;
    }
    // GIF：GIF8，LSD 宽高 LE @6/@8
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        w = data[6] | (static_cast<uint32_t>(data[7]) << 8);
        h = data[8] | (static_cast<uint32_t>(data[9]) << 8);
        return w > 0 && h > 0;
    }
    // JPEG：FF D8，扫描 SOF0/1/2 段（段内高 BE 在前、宽在后）
    if (data[0] == 0xFF && data[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 9 <= data.size()) {
            if (data[pos] != 0xFF) break;
            const uint8_t marker = data[pos + 1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                h = (static_cast<uint32_t>(data[pos + 5]) << 8) | data[pos + 6];
                w = (static_cast<uint32_t>(data[pos + 7]) << 8) | data[pos + 8];
                return w > 0 && h > 0;
            }
            if (pos + 4 > data.size()) break;
            // 其他段：2 字节段长（含自身）整段跳过
            const size_t seg_len = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
            if (seg_len < 2) break;
            pos += 2 + seg_len;
        }
        return false;
    }
    // BMP：BM，宽高 LE int32 @18/@22（height 可为负=顶向下，取绝对值）
    if (data[0] == 'B' && data[1] == 'M' && data.size() >= 26) {
        w = data[18] | (static_cast<uint32_t>(data[19]) << 8) |
            (static_cast<uint32_t>(data[20]) << 16) | (static_cast<uint32_t>(data[21]) << 24);
        h = data[22] | (static_cast<uint32_t>(data[23]) << 8) |
            (static_cast<uint32_t>(data[24]) << 16) | (static_cast<uint32_t>(data[25]) << 24);
        if (w & 0x80000000u) w = ~w + 1;
        if (h & 0x80000000u) h = ~h + 1;
        return w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF;
    }
    return false;
}
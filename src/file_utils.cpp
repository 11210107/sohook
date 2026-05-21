//
// Created by user_wangzhen on 2026/5/20.
//
#include "file_utils.h"
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
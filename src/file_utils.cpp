//
// Created by user_wangzhen on 2026/5/20.
//
#include <cstdint>
#include <fstream>
#include "file_utils.h"
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
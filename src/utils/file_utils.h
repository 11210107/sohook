//
// Created by user_wangzhen on 2026/5/20.
//

#ifndef SOHOOK_FILE_UTILS_H
#define SOHOOK_FILE_UTILS_H
#include <cstdint>
#include <jni.h>
#include <string>
#include <vector>
std::vector<uint8_t> read_binary_file(const std::string& file_path);
uint64_t get_file_size(const std::string& file_path);
bool get_image_info(const std::string& path, uint64_t& out_size, uint32_t& out_width, uint32_t& out_height);
std::string resolve_content_uri_to_local(JNIEnv *env,const std::string &uri_str);
#endif //SOHOOK_FILE_UTILS_H
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
std::string prepare_sandbox_file(const std::string& src_path, uint64_t conv_id);
bool get_image_info(const std::string& path, uint64_t& out_size, uint32_t& out_width, uint32_t& out_height);
bool mkdir_p(const std::string& path);
std::string copy_to_sandbox_via_jni(JNIEnv* env, const std::string& src_path);
#endif //SOHOOK_FILE_UTILS_H
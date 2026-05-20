//
// Created by user_wangzhen on 2026/4/14.
//
#include <iostream>
#include <dlfcn.h>
#include <unistd.h>
using namespace std;

int main() {
    // 定义 so 文件的路径
    const char* so_path = "/data/local/tmp/libsohook.so";
    cout << "[Loader] 准备手动加载库：" << so_path << endl;

    // 调用 dlopen 加载库，RTLD_NOW 表示立即解析库中所有的符号
    void* handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        // 如果加载失败，打印错误原因
        cerr << "[Loader] 加载失败：" << dlerror() << endl;
        return 1;
    }
    cout << "[Loader] 库已经加载到内存，地址：" << handle << endl;

    // 此刻，libsohook.so 里的 __attribute__((constructor))应该已经执行了
    // 模拟程序继续运行
    cout << "[Loader] 正在执行main方法" << endl;
    auto main = (int(*)())dlsym(handle, "main");
    if (main) {
        cout << "[Loader] 开始主动调用库里的 main 函数：" << endl;
        main();
    }
    sleep(2);
    // 卸载库（通常在程序退出前调用）
    dlclose(handle);
    cout << "[Loader] 程序退出。" << endl;
    return 0;
}


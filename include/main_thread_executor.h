//
// Created by user_wangzhen on 2026/7/15.
//

#ifndef MAIN_THREAD_EXECUTOR_H
#define MAIN_THREAD_EXECUTOR_H

#include <android/looper.h>
#include <functional>
#include <mutex>
class MainThreadExecutor {
private:
    int m_pipe_fds[2];
    ALooper* m_main_looper;
    std::mutex m_write_mutex; // 确保多线程并发 post 安全
    // 单例模式：构造与析构私有化
    MainThreadExecutor();
    ~MainThreadExecutor();

    // 禁止拷贝和赋值
    MainThreadExecutor(const MainThreadExecutor&) = delete;
    MainThreadExecutor& operator=(const MainThreadExecutor&) = delete;

    // 内部的 Looper 静态回调
    static int looperCallback(int fd, int events, void* data);

public:
    // 获取全局单例引用
    static MainThreadExecutor& getInstance();

    // 初始化：必须在主线程调用
    bool initialize();

    // 投递任务到主线程执行
    void post(std::function<void()> task);
};

#endif // MAIN_THREAD_EXECUTOR_H
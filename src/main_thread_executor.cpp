//
// Created by user_wangzhen on 2026/7/15.
//
#include "main_thread_executor.h"
#include <unistd.h>
#include <fcntl.h>
#include <utility>
#include <sys/syscall.h>
#include "logger.h"
// 辅助函数：获取当前 Linux 线程 ID
static pid_t get_current_tid() {
    return syscall(SYS_gettid);
}
MainThreadExecutor::MainThreadExecutor()
    : m_main_looper(nullptr) {
    m_pipe_fds[0] = -1;
    m_pipe_fds[1] = -1;
    LOGI("MainThreadExecutor: created");
}

MainThreadExecutor::~MainThreadExecutor() {
    // 析构时清理管道描述符
    if (m_pipe_fds[0] != -1) close(m_pipe_fds[0]);
    if (m_pipe_fds[1] != -1) close(m_pipe_fds[1]);
    LOGI("~MainThreadExecutor: Cleaned up pipe fds.");
}

MainThreadExecutor &MainThreadExecutor::getInstance() {
    static MainThreadExecutor instance;
    return instance;
}


bool MainThreadExecutor::initialize() {
    pid_t tid = get_current_tid();
    pid_t pid = getpid();

    LOGI("initialize() called. Current TID: %d, Process PID: %d", tid, pid);
    m_main_looper = ALooper_forThread();
    if (!m_main_looper) {
        LOGI("ALooper_forThread() returned NULL. Trying ALooper_prepare(0)...");
        m_main_looper = ALooper_prepare(0); // 0 表示默认配置
    }

    if (!m_main_looper) {
        LOGE("FAILED to initialize! Even ALooper_prepare(0) returned NULL.");
        return false;
    }
    // 创建带 O_CLOEXEC 和 O_NONBLOCK 标志的管道
    if (pipe2(m_pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        LOGE("FAILED to create pipe! errno: %d", errno);
        return false;
    }

    // 将管道读端（read_fd）加入主线程的 Looper 监听
    int result = ALooper_addFd(
        m_main_looper,
        m_pipe_fds[0], // fd
        ALOOPER_POLL_CALLBACK, // ident
        ALOOPER_EVENT_INPUT, // 监听可读事件
        looperCallback, // 回调函数
        nullptr // 用户数据指针
    );
    if (result == 1) {
        LOGI("MainThreadExecutor initialized successfully! Hooked into ALooper.");
        return true;
    } else {
        LOGE("ALooper_addFd failed! result code: %d", result);
        return false;
    }
}

void MainThreadExecutor::post(std::function<void()> task) {
    if (m_pipe_fds[1] == -1) {
        LOGE("post() ignored! Executor not initialized.");
        return;
    }

    auto *task_ptr = new std::function<void()>(std::move(task));

    {
        // 【防崩溃加固：加锁防止并发写导致数据错位】
        std::lock_guard<std::mutex> lock(m_write_mutex);

        ssize_t written = write(m_pipe_fds[1], &task_ptr, sizeof(task_ptr));
        if (written != sizeof(task_ptr)) {
            LOGE("post() - Write failed, corrupted pointer! errno: %d", errno);
            delete task_ptr;
        }
    }
}

int MainThreadExecutor::looperCallback(int fd, int events, void *data) {
    LOGD("looperCallback triggered! FD: %d, Events: %d", fd, events);

    if (events & ALOOPER_EVENT_INPUT) {
        std::function<void()> *task = nullptr;

        // 读取指针
        if (read(fd, &task, sizeof(task)) == sizeof(task)) {
            if (task && *task) {
                // 【防崩溃加固：异常保护拦截】
                try {
                    (*task)();
                } catch (const std::exception& e) {
                    LOGE("looperCallback - Caught standard exception inside task: %s", e.what());
                } catch (...) {
                    LOGE("looperCallback - Caught unknown exception inside task!");
                }
            }
            delete task;
        }
    }
    return 1;
}

/**
 * @file crash_reporter.h
 * @brief 崩溃诊断：拦截 assert/abort、std::terminate 与 SEH 异常，
 *        退出瞬间把异常内容与符号化堆栈写入 ~/.workx/logs/。
 * @details Debug 构建 + /RTC1 下越界/断言常走 abort()（退出码 3，无 WER 弹窗），
 *          普通 try/catch 捕获不到。此模块安装全局处理器以获得确切崩溃来源。
 */
#pragma once

namespace crash {

/// @brief 安装全部崩溃处理器（SEH 异常 / std::terminate / SIGABRT / CRT 断言）。
void InstallHandlers();

/// @brief 在 FTXUI 初始化（install 信号处理器）之后重装 CRT/信号处理器。
void ReinstallSignalHandlers();

/// @brief 手动触发一次诊断转储（供调用方主动上报）。
void DumpNow(const char* reason);

}  // namespace crash
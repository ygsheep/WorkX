/**
 * @file permission_ask.cpp
 * @brief 权限确认工具实现
 * @details 发布 AskUserRequestEvent（带 result_promise），等待宿主填回
 *          AskUserResult；答案中任一项为 "Yes"（不区分大小写）即放行。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/permission_ask.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

namespace {

bool is_yes(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "yes";
}

} // namespace

bool ask_user_confirm(
    const ToolContext& ctx,
    const std::string& question,
    int timeout_ms
) {
    if (!ctx.event_bus_ptr) return false;  // fail-closed：无确认通道即拒绝

    auto request = std::make_shared<AskUserRequestEvent>();
    auto wait_promise = std::make_shared<std::promise<AskUserResult>>();
    request->session_id = ctx.session_id;
    request->timeout_ms = timeout_ms;
    request->result_promise = wait_promise;
    request->cancel_flag = std::make_shared<std::atomic<bool>>(false);
    request->questions = nlohmann::json::array({
        {
            {"question", question},
            {"header", "Permission Required"},
            {"options", {"Yes", "No"}},
        }
    });

    // 发布到总线；宿主（TUI）订阅 AskUserRequestEvent 后弹出确认面板
    ctx.event_bus_ptr->publish_async(request);

    auto future = wait_promise->get_future();
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) !=
        std::future_status::ready) {
        return false;  // 超时/宿主未响应：拒绝
    }
    const AskUserResult result = future.get();
    if (!result.submitted) return false;
    for (const auto& [question_key, answer] : result.answers) {
        (void)question_key;
        if (is_yes(answer)) return true;
    }
    return false;
}

bool is_plan_mode(PermissionMode mode) noexcept {
    return mode == PermissionMode::Plan;
}

bool is_bypass_mode(PermissionMode mode) noexcept {
    return mode == PermissionMode::BypassPermissions;
}

bool deny_write_by_mode(PermissionMode mode) noexcept {
    return is_plan_mode(mode);
}

bool deny_execute_by_mode(PermissionMode mode) noexcept {
    return is_plan_mode(mode);
}

bool is_dangerous_command(const std::string& command) noexcept {
    static const std::vector<std::string> kPatterns = {
        // 破坏性文件操作
        "rm -rf", "rm -fr", "rm -r /", "rm -f /",
        "rmdir /s", "del /s", "del /f", "rd /s",
        "chmod 777", "chmod -r 777", "chown -r",
        // 磁盘/系统级
        "mkfs", "fdisk", "dd if=", "format ",
        "shutdown", "reboot", "halt", "poweroff",
        // 管道到 shell（远程执行模式）
        "| bash", "| sh ", "| zsh", "| powershell",
    };
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& pattern : kPatterns) {
        if (lower.find(pattern) != std::string::npos) return true;
    }
    return false;
}

} // namespace agent::tool
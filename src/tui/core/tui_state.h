/**
 * @file tui_state.h
 * @brief TUI 状态机
 * @details 定义 TuiState 枚举和状态转换逻辑
 * @version 2.0.0
 */

#pragma once

#include <string_view>

namespace agent {

/**
 * @brief TUI 状态
 */
enum class TuiState {
    IDLE,           ///< 等待用户输入
    THINKING,       ///< 模型思考中（显示推理块 + 计时器）
    STREAMING,      ///< 流式输出内容
    TOOL_RUNNING,   ///< 工具调用执行中
    ERROR,          ///< 错误状态
    DETAIL_VIEW     ///< Ctrl+O 详情视图（拦截输入用于滚动）
};

/**
 * @brief 获取 TuiState 的显示名称
 */
constexpr std::string_view tui_state_name(TuiState state) {
    switch (state) {
        case TuiState::IDLE:         return "idle";
        case TuiState::THINKING:     return "thinking";
        case TuiState::STREAMING:    return "streaming";
        case TuiState::TOOL_RUNNING: return "tool";
        case TuiState::ERROR:        return "error";
        case TuiState::DETAIL_VIEW:  return "detail";
        default:                     return "unknown";
    }
}

/**
 * @brief TUI 状态机
 * @details 管理状态转换，验证转换合法性
 */
class TuiStateMachine {
public:
    TuiStateMachine() = default;

    /// @brief 获取当前状态
    TuiState current() const { return m_state; }

    /// @brief 尝试转换到新状态
    /// @return true 转换成功
    bool transition_to(TuiState new_state) {
        if (!is_valid_transition(m_state, new_state)) {
            return false;
        }
        m_state = new_state;
        return true;
    }

    /// @brief 强制设置状态（用于中断/错误等场景）
    void force_state(TuiState new_state) {
        m_state = new_state;
    }

    /// @brief 验证状态转换是否合法
    static bool is_valid_transition(TuiState from, TuiState to) {
        if (from == to) return true;

        // DETAIL_VIEW 是叠加态，可从任何状态进入，也可回到任何状态
        // （实际上由 ChatRenderer 直接 force_state 切换，不走 transition_to）
        if (to == TuiState::DETAIL_VIEW) return true;
        if (from == TuiState::DETAIL_VIEW) return true;

        switch (from) {
            case TuiState::IDLE:
                return to == TuiState::THINKING;

            case TuiState::THINKING:
                return to == TuiState::STREAMING
                    || to == TuiState::TOOL_RUNNING
                    || to == TuiState::ERROR
                    || to == TuiState::IDLE;      // interrupt

            case TuiState::STREAMING:
                return to == TuiState::IDLE       // stream_done
                    || to == TuiState::TOOL_RUNNING
                    || to == TuiState::THINKING   // reasoning_delta
                    || to == TuiState::ERROR;

            case TuiState::TOOL_RUNNING:
                return to == TuiState::STREAMING
                    || to == TuiState::IDLE       // stream_done
                    || to == TuiState::ERROR;

            case TuiState::ERROR:
                return to == TuiState::IDLE       // any input clears error
                    || to == TuiState::THINKING;  // retry

            case TuiState::DETAIL_VIEW:
                return true;  // 已在前面处理，兜底
        }
        return false;
    }

private:
    TuiState m_state = TuiState::IDLE;
};

} // namespace workx

/**
 * @file vt_input_decoder.h
 * @brief VT 终端输入序列解码器
 * @details 把方向键/功能键/bracketed paste 标记解码为统一事件。
 *          纯逻辑、无平台依赖，Win32/POSIX 平台共用，可独立单测。
 * @version 1.0.0
 */

#pragma once

#include <cstdint>
#include <string>

namespace tui {

/// @brief 解码后的事件码（与各平台 KEY_* 常量保持一致）
constexpr char32_t VT_KEY_ARROW_LEFT       = 0xE000;
constexpr char32_t VT_KEY_ARROW_RIGHT      = 0xE001;
constexpr char32_t VT_KEY_ARROW_UP         = 0xE002;
constexpr char32_t VT_KEY_ARROW_DOWN       = 0xE003;
constexpr char32_t VT_KEY_HOME             = 0xE004;
constexpr char32_t VT_KEY_END              = 0xE005;
constexpr char32_t VT_KEY_CTRL_ARROW_LEFT  = 0xE006;
constexpr char32_t VT_KEY_CTRL_ARROW_RIGHT = 0xE007;
constexpr char32_t VT_KEY_DELETE           = 0xE008;

/**
 * @brief VT 输入序列解码器
 */
class VtInputDecoder {
public:
    enum class Event {
        None,        ///< 序列中间态，无输出（继续喂入后续码点）
        Char,        ///< 普通字符（经 code() 读取；粘贴内换行已转换为 '\n'）
        PasteBegin,  ///< bracketed paste 开始（ESC[200~）
        PasteEnd,    ///< bracketed paste 结束（ESC[201~）
    };

    /// @brief 喂入一个码点，返回事件
    Event feed(char32_t cp);

    /// @brief Char 事件对应的码点
    char32_t code() const { return m_code; }

    /// @brief 是否处于 bracketed paste 粘贴模式
    bool paste_active() const { return m_paste_active; }

    /// @brief 重置解码状态（丢弃半截序列，如跨线程唤醒打断 ESC/CSI 时）
    void reset() { m_state = State::Idle; m_params.clear(); }

private:
    enum class State { Idle, Esc, Ss3, Csi, CsiParam };
    State m_state = State::Idle;
    std::string m_params;      ///< CSI 参数缓冲（如 "1;5"）
    bool m_paste_active = false;
    bool m_last_was_cr_in_paste = false;  ///< 粘贴内上一个字符是 \r（\r\n 归一化）
    char32_t m_code = 0;

    Event finish(char32_t final_byte);
};

} // namespace tui

/**
 * @file vt_input_decoder.cpp
 * @brief VT 输入序列解码器实现
 * @version 1.0.0
 */

#include "tui/core/platform/vt_input_decoder.h"

namespace tui {

VtInputDecoder::Event VtInputDecoder::feed(char32_t cp) {
    m_code = 0;
    switch (m_state) {
        case State::Idle:
            // 粘贴模式：换行（\r/\n）统一为 '\n'（插入换行而非提交），
            // 制表符转空格（避免触发补全/焦点跳转）；\r\n 连续视为单个换行
            if (m_paste_active) {
                if (cp == '\r') {
                    m_last_was_cr_in_paste = true;
                    m_code = '\n';
                    return Event::Char;
                }
                if (cp == '\n') {
                    if (m_last_was_cr_in_paste) {
                        // \r\n：跳过紧随 \r 的 \n（归一化为单个换行）
                        m_last_was_cr_in_paste = false;
                        return Event::None;
                    }
                    m_code = '\n';
                    return Event::Char;
                }
                m_last_was_cr_in_paste = false;
                if (cp == '\t') {
                    m_code = ' ';
                    return Event::Char;
                }
            }
            if (cp == 0x1B) {
                m_state = State::Esc;
                return Event::None;
            }
            m_code = cp;
            return Event::Char;

        case State::Esc:
            if (cp == '[') {
                m_state = State::Csi;
                m_params.clear();
                return Event::None;
            }
            if (cp == 'O') {
                m_state = State::Ss3;
                return Event::None;
            }
            // 非序列的 ESC：返回 ESC 本身，丢弃后续字符（与既有行为一致）
            reset();
            m_code = 0x1B;
            return Event::Char;

        case State::Ss3:
            reset();
            switch (cp) {
                case 'H': m_code = VT_KEY_HOME; return Event::Char;
                case 'F': m_code = VT_KEY_END;  return Event::Char;
                default:  return Event::None;
            }

        case State::Csi:
            if ((cp >= '0' && cp <= '9') || cp == ';') {
                m_state = State::CsiParam;
                m_params += static_cast<char>(cp);
                return Event::None;
            }
            return finish(cp);

        case State::CsiParam:
            if ((cp >= '0' && cp <= '9') || cp == ';') {
                m_params += static_cast<char>(cp);
                return Event::None;
            }
            return finish(cp);
    }
    return Event::None;
}

VtInputDecoder::Event VtInputDecoder::finish(char32_t final_byte) {
    const std::string params = m_params;
    const bool ctrl_mod = (params == "1;5");
    reset();
    switch (final_byte) {
        case 'A': m_code = VT_KEY_ARROW_UP; return Event::Char;
        case 'B': m_code = VT_KEY_ARROW_DOWN; return Event::Char;
        case 'C': m_code = ctrl_mod ? VT_KEY_CTRL_ARROW_RIGHT : VT_KEY_ARROW_RIGHT; return Event::Char;
        case 'D': m_code = ctrl_mod ? VT_KEY_CTRL_ARROW_LEFT : VT_KEY_ARROW_LEFT; return Event::Char;
        case 'H': m_code = VT_KEY_HOME; return Event::Char;
        case 'F': m_code = VT_KEY_END; return Event::Char;
        case 'Z': m_code = VT_KEY_BACKTAB; return Event::Char;  // Shift+Tab
        case '~':
            if (params == "200") { m_paste_active = true; return Event::PasteBegin; }
            if (params == "201") { m_paste_active = false; return Event::PasteEnd; }
            if (params == "1" || params == "7" || params == "8") { m_code = VT_KEY_HOME; return Event::Char; }
            if (params == "5") { m_code = VT_KEY_HOME; return Event::Char; }  // PgUp
            if (params == "4") { m_code = VT_KEY_END; return Event::Char; }
            if (params == "3") { m_code = VT_KEY_DELETE; return Event::Char; }
            return Event::None;  // 其余（如 PgDn）丢弃
        default:
            return Event::None;  // 未知序列：丢弃，避免状态卡死
    }
}

} // namespace tui

#include "command/command_registry.h"

#include <utility>

namespace ftxtui {

void CommandRegistry::add(PaletteCommand cmd) {
    entries_.push_back(std::move(cmd));
}

CommandRegistry CommandRegistry::builtins() {
    CommandRegistry reg;
    // command, title, keywords（中英文搜索别名）
    reg.add({"/help",   "帮助",       "help, 帮助"});
    reg.add({"/clear",  "清空会话",    "clear, 清空"});
    reg.add({"/exit",   "退出",       "exit, quit, 退出"});
    reg.add({"/model",  "切换模型",    "switch model, model, 切换模型, 模型"});
    reg.add({"/resume", "恢复会话",    "resume, 回复, 恢复, 会话"});
    reg.add({"/rename", "重命名会话",  "rename, 重命名, 标题"});
    return reg;
}

}  // namespace ftxtui
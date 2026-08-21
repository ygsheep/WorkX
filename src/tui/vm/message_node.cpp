#include "vm/message_node.h"

#include <algorithm>

namespace ftxtui {

ToolCallNode* MessageNode::find_tool(const std::string& call_id) {
    auto it = std::find_if(tool_calls.begin(), tool_calls.end(),
                           [&](const ToolCallNode& t) { return t.call_id == call_id; });
    return it == tool_calls.end() ? nullptr : &*it;
}

const ToolCallNode* MessageNode::find_tool(const std::string& call_id) const {
    auto it = std::find_if(tool_calls.begin(), tool_calls.end(),
                           [&](const ToolCallNode& t) { return t.call_id == call_id; });
    return it == tool_calls.end() ? nullptr : &*it;
}

}  // namespace ftxtui
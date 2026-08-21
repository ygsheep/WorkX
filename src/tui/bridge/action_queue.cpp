#include <utility>

#include "bridge/action_queue.h"

namespace ftxtui {

void ActionQueue::push(Action action) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(action));
}

std::deque<Action> ActionQueue::drain() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::exchange(m_queue, {});
}

bool ActionQueue::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
}

}  // namespace ftxtui
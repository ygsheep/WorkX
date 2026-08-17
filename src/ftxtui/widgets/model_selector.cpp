#include "widgets/model_selector.h"

#include <ftxui/component/event.hpp>

namespace ftxtui {

using ftxui::Component;
using ftxui::Event;

Component make_model_selector(std::vector<std::string>& models,
                              std::function<void(int)> on_select,
                              bool& open) {
    int selected = 0;

    Component menu = ftxui::Menu(&models, &selected);
    Component modal = menu | ftxui::CatchEvent([&](Event e) {
        if (e == Event::Return) {
            if (!models.empty() && selected >= 0 && static_cast<size_t>(selected) < models.size()) {
                if (on_select) on_select(selected);
                open = false;
            }
            return true;
        }
        if (e == Event::Escape) {
            open = false;
            return true;
        }
        return false;
    });
    modal |= ftxui::Maybe([&] { return open; });

    return modal;
}

}  // namespace ftxtui
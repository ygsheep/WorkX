/**
 * @file path_completer.cpp
 * @brief 文件路径 Tab 补全实现
 * @version 1.0.0
 * @date 2026-07
 */

#include <filesystem>
#include <system_error>

#include "app/ui/path_completer.h"

namespace agent {

std::vector<std::pair<std::string, size_t>> complete_file_path(
    std::string_view prefix, size_t cursor_pos)
{
    std::vector<std::pair<std::string, size_t>> results;

    std::string_view path_part = prefix.substr(0, cursor_pos);

    auto last_space = path_part.rfind(' ');
    std::string_view path_token = (last_space == std::string_view::npos)
        ? path_part : path_part.substr(last_space + 1);

    if (path_token.empty()) return results;

    namespace fs = std::filesystem;
    std::string dir_str;
    std::string file_prefix;

    auto last_sep = path_token.rfind('/');
#ifdef _WIN32
    auto last_sep_win = path_token.rfind('\\');
    if (last_sep_win != std::string_view::npos &&
        (last_sep == std::string_view::npos || last_sep_win > last_sep)) {
        last_sep = last_sep_win;
    }
#endif

    if (last_sep == std::string_view::npos) {
        dir_str = ".";
        file_prefix = std::string(path_token);
    } else {
        dir_str = std::string(path_token.substr(0, last_sep + 1));
        file_prefix = std::string(path_token.substr(last_sep + 1));
    }

    if (dir_str.empty()) dir_str = ".";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_str, ec)) {
        if (ec) break;

        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;

        if (name.compare(0, file_prefix.size(), file_prefix) == 0) {
            std::string before_token(path_part.substr(0, path_token.data() - path_part.data()));
            std::string completed = before_token + std::string(path_token.substr(0, last_sep == std::string_view::npos ? 0 : last_sep + 1)) + name;

            if (entry.is_directory()) {
                completed += '/';
            }

            std::string full_line = std::string(prefix.substr(0, path_token.data() - prefix.data()))
                + completed
                + std::string(prefix.substr(cursor_pos));

            results.push_back({full_line, path_token.data() - prefix.data() + completed.size()});
            if (results.size() >= 20) break;
        }
    }

    return results;
}

} // namespace workx

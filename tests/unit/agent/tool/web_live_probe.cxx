// web_live_probe.cxx — 国内站点 WebFetch/WebSearch 实测探针（手动运行，不入 ctest）
// 构建：cmake --build build --target web_live_probe --config Debug
// 运行：build/bin/Debug/web_live_probe.exe
#include <cstdio>
#include <cstdlib>
#include <string>

#include "agent/tool/WebFetchTool/web_fetch_tool.h"
#include "agent/tool/WebSearchTool/web_search_tool.h"
#include "agent/config/app_config.h"
#include "core/config/config_manager.h"

using namespace agent;
using namespace agent::tool;

static void probe_fetch(const char* label, const char* url) {
    std::printf("===== WebFetch [%s] %s =====\n", label, url);
    WebFetchTool t;
    ToolContext ctx;
    ctx.permission_mode = PermissionMode::BypassPermissions;
    auto r = t.call({{"url", url}}, ctx);
    if (r.is_err()) {
        std::printf("  ERROR: %s\n", r.error().message.c_str());
        return;
    }
    const std::string& text = r.value().text;
    std::printf("  OK, %zu chars\n", text.size());
    std::printf("  PREVIEW: %.700s\n", text.c_str());
    std::printf("\n");
}

static void probe_search(const char* query) {
    std::printf("===== WebSearch [%s] =====\n", query);
    WebSearchTool t;
    ToolContext ctx;
    ctx.permission_mode = PermissionMode::BypassPermissions;
    ctx.config_manager_ptr = &ConfigManager::instance();
    auto r = t.call({{"query", query}, {"num_results", 5}}, ctx);
    if (r.is_err()) {
        std::printf("  ERROR: %s\n", r.error().message.c_str());
        return;
    }
    const std::string& text = r.value().text;
    std::printf("  OK, %zu chars\n", text.size());
    std::printf("  PREVIEW: %.1200s\n", text.c_str());
    std::printf("\n");
}

int main() {
    std::printf("=== WorkX 国内站点 WebFetch/WebSearch 实测探针 ===\n\n");

    // 显式用 Bing 免 Key 搜索，跳过公共 SearXNG 实例超时
    register_config_defaults(ConfigManager::instance());
    ConfigManager::instance().set(keys::WEB_SEARCH_PROVIDER, std::string("bing"));

    // 1. WebFetch：文章/页面 → Markdown
    probe_fetch("博客园(文章)", "https://www.cnblogs.com/hez2010/p/22444617");
    probe_fetch("稀土掘金(文章)", "https://juejin.cn/post/7644802673648140340");
    probe_fetch("知乎(问题页)", "https://www.zhihu.com/question/19787904");
    probe_fetch("知识星球(首页)", "https://wx.zsxq.com/");

    // 2. WebSearch：Bing 免 Key
    probe_search("知乎 知识星球 博客园 稀土掘金 C++ 教程");
    probe_search("site:cnblogs.com nlohmann json 使用");
    probe_search("掘金 2026 AI 编程 文章");
    return 0;
}

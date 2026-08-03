/**
 * @file test_choice_panel.cpp
 * @brief ChoicePanel 配置解析单元测试 (M-5)
 * @details 覆盖 parse_choice_config 的成功/失败/边界路径。
 *          不测试 run_choice_panel（依赖 Terminal 平台层，留待集成测试）。
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/widgets/choice_panel.h"

#include <nlohmann/json.hpp>

using namespace tui;
using json = nlohmann::json;

// ============================================================
// 成功路径
// ============================================================

TEST_CASE("parse_choice_config: single question single choice (default allow_custom_input=true)", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "选择哪种修复策略？"},
                {"header", "策略"},
                {"multiSelect", false},
                {"options", json::array({
                    {{"label", "重构"}, {"description", "提取公共函数"}},
                    {{"label", "打补丁"}, {"description", "最小改动"}},
                    {{"label", "重写"}, {"description", "整体重写"}}
                })}
            }
        })}
    };

    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs.size() == 1);

    const auto& tab = config->tabs[0];
    REQUIRE(tab.question == "选择哪种修复策略？");
    REQUIRE(tab.header == "策略");
    REQUIRE(tab.multi == false);
    REQUIRE(tab.allow_custom_input == true);  // 默认 true
    REQUIRE(tab.items.size() == 3);
    REQUIRE(tab.items[0].label == "重构");
    REQUIRE(tab.items[0].id == "重构");  // id == label
    REQUIRE(tab.items[0].description == "提取公共函数");
    REQUIRE(tab.items[1].label == "打补丁");
    REQUIRE(tab.items[2].label == "重写");
}

TEST_CASE("parse_choice_config: single question multi choice allow_custom_input=false", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "选择要修改的文件？"},
                {"header", "文件"},
                {"multiSelect", true},
                {"allow_custom_input", false},
                {"options", json::array({
                    {{"label", "src/app/main.cpp"}},
                    {{"label", "src/agent/core/chat_session.cpp"}}
                })}
            }
        })}
    };

    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs.size() == 1);

    const auto& tab = config->tabs[0];
    REQUIRE(tab.multi == true);
    REQUIRE(tab.allow_custom_input == false);
    REQUIRE(tab.items.size() == 2);
    // description 缺省应为空
    REQUIRE(tab.items[0].description.empty());
    REQUIRE(tab.items[0].id == "src/app/main.cpp");
}

TEST_CASE("parse_choice_config: multi-tab multi-question mixed config", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "选择哪种修复策略？"},
                {"header", "策略"},
                {"multiSelect", false},
                {"options", json::array({
                    {{"label", "重构"}, {"description", "提取公共函数"}},
                    {{"label", "打补丁"}, {"description", "最小改动"}}
                })}
            },
            {
                {"question", "选择要修改的文件？"},
                {"header", "文件"},
                {"multiSelect", true},
                {"options", json::array({
                    {{"label", "src/app/main.cpp"}},
                    {{"label", "src/app/factory.cpp"}}
                })}
            },
            {
                {"question", "需要运行哪些验证？"},
                {"header", "测试"},
                {"multiSelect", true},
                {"allow_custom_input", false},
                {"options", json::array({
                    {{"label", "单元测试"}},
                    {{"label", "集成测试"}}
                })}
            }
        })}
    };

    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs.size() == 3);

    REQUIRE(config->tabs[0].header == "策略");
    REQUIRE(config->tabs[0].multi == false);
    REQUIRE(config->tabs[0].allow_custom_input == true);  // 默认

    REQUIRE(config->tabs[1].header == "文件");
    REQUIRE(config->tabs[1].multi == true);
    REQUIRE(config->tabs[1].allow_custom_input == true);  // 默认

    REQUIRE(config->tabs[2].header == "测试");
    REQUIRE(config->tabs[2].multi == true);
    REQUIRE(config->tabs[2].allow_custom_input == false);  // 显式 false
}

TEST_CASE("parse_choice_config: multiSelect defaults to false", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({{{"label", "A"}}, {{"label", "B"}}})}
            }
        })}
    };

    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs[0].multi == false);  // 缺省 false
    REQUIRE(config->tabs[0].allow_custom_input == true);  // 缺省 true
}

TEST_CASE("parse_choice_config: id equals label", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({
                    {{"label", "选项A"}},
                    {{"label", "选项B"}}
                })}
            }
        })}
    };

    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs[0].items[0].id == "选项A");
    REQUIRE(config->tabs[0].items[1].id == "选项B");
}

// ============================================================
// 失败路径 (返回 nullopt)
// ============================================================

TEST_CASE("parse_choice_config: missing questions key returns nullopt", "[choice_panel][parse]") {
    json input = {{"foo", "bar"}};
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: questions not an array returns nullopt", "[choice_panel][parse]") {
    json input = {{"questions", "not-an-array"}};
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: questions empty array returns nullopt", "[choice_panel][parse]") {
    json input = {{"questions", json::array()}};
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: question missing question field returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"header", "H"},
                {"options", json::array({{{"label", "A"}}})}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: question missing header field returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"options", json::array({{{"label", "A"}}})}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: question missing options field returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {{"question", "Q?"}, {"header", "H"}}
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: option missing label field returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({
                    {{"description", "no label here"}}
                })}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: empty options array returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array()}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

// ============================================================
// 类型错误 (返回 nullopt)
// ============================================================

TEST_CASE("parse_choice_config: question not a string returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", 123},
                {"header", "H"},
                {"options", json::array({{{"label", "A"}}})}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: header not a string returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", true},
                {"options", json::array({{{"label", "A"}}})}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: label not a string returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({
                    {{"label", 42}}
                })}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: options not an array returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", "not-array"}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

// ============================================================
// 异常路径
// ============================================================

TEST_CASE("parse_choice_config: input not an object returns nullopt", "[choice_panel][parse]") {
    json input = json::array({1, 2, 3});
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: question element not an object returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({"not-an-object", 42})}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

TEST_CASE("parse_choice_config: option element not an object returns nullopt", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({"string-option", 123})}
            }
        })}
    };
    REQUIRE_FALSE(parse_choice_config(input).has_value());
}

// ============================================================
// 边界：description 为非字符串时被忽略 (value() 安全处理)
// ============================================================

TEST_CASE("parse_choice_config: description non-string is ignored", "[choice_panel][parse]") {
    json input = {
        {"questions", json::array({
            {
                {"question", "Q?"},
                {"header", "H"},
                {"options", json::array({
                    {{"label", "A"}, {"description", 123}}  // description 非字符串
                })}
            }
        })}
    };
    // parse_choice_config 用 is_string() 判断 description，非字符串时跳过赋值，不报错
    auto config = parse_choice_config(input);
    REQUIRE(config.has_value());
    REQUIRE(config->tabs[0].items[0].description.empty());
}

# Cppcheck 分析报告

| 文件 | 行 | 列 | 严重度 | 消息 | ID |
|------|----|----|--------|------|----|
| src\agent\api\client.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\agent\api\remote\http_client.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\agent\api\sse_parser.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\agent\command\source\executor.cpp | 44 | 20 | style | Variable 'block' can be declared as reference to const | constVariableReference |
| src\agent\command\source\executor.cpp | 39 | 15 | style | Variable 'local_cmd' can be declared as pointer to const | constVariablePointer |
| src\agent\command\source\executor.cpp | 41 | 22 | style | Variable 'prompt_cmd' can be declared as pointer to const | constVariablePointer |
| src\agent\command\source\registry.cpp | 27 | 19 | style | Consider using std::copy_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\command\source\registry.cpp | 37 | 19 | style | Consider using std::copy_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\command\source\registry.cpp | 59 | 19 | style | Consider using std::copy_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\command\source\registry.cpp | 69 | 19 | style | Consider using std::copy_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\model\provider_preset.cpp | 76 | 34 | style | Consider using std::find_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\model\provider_preset.cpp | 87 | 15 | style | Consider using std::transform algorithm instead of a raw loop. | useStlAlgorithm |
| src\agent\tool\GlobTool\glob_tool.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\app\config\app_config.cpp | 118 | 25 | style | Variable 'val' is assigned a value that is never used. | unreadVariable |
| src\app\config\cli_args.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\app\main.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\app\main.cpp | 326 | 40 | warning | Either the condition 'registry' is redundant or there is possible null pointer dereference: registry. | nullPointerRedundantCheck |
| src\app\ui\model_selector.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\app\ui\model_selector.cpp | 74 | 25 | style | Consider using std::transform algorithm instead of a raw loop. | useStlAlgorithm |
| src\app\ui\model_selector.cpp | 90 | 15 | style | Consider using std::transform algorithm instead of a raw loop. | useStlAlgorithm |
| src\app\ui\model_selector.cpp | 98 | 43 | style | Consider using std::find_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\app\ui\model_selector.cpp | 31 | 31 | style | Variable 'KEY_UP' is assigned a value that is never used. | unreadVariable |
| src\app\ui\model_selector.cpp | 32 | 31 | style | Variable 'KEY_DOWN' is assigned a value that is never used. | unreadVariable |
| src\app\ui\model_selector.cpp | 33 | 31 | style | Variable 'KEY_TAB' is assigned a value that is never used. | unreadVariable |
| src\app\ui\model_selector.cpp | 34 | 31 | style | Variable 'KEY_ENTER' is assigned a value that is never used. | unreadVariable |
| src\app\ui\model_selector.cpp | 36 | 31 | style | Variable 'KEY_SPACE' is assigned a value that is never used. | unreadVariable |
| src\app\ui\model_selector.cpp | 37 | 31 | style | Variable 'KEY_CTRL_C' is assigned a value that is never used. | unreadVariable |
| src\app\ui\path_completer.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\core\task\task_manager.cpp | 150 | 39 | style | Consider using std::copy_if algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\core\platform\platform_posix.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\core\platform\platform_posix.cpp | 177 | 15 | style | Variable 'cols' can be declared as pointer to const | constVariablePointer |
| src\tui\core\platform\platform_posix.cpp | 190 | 15 | style | Variable 'rows' can be declared as pointer to const | constVariablePointer |
| src\tui\core\platform\platform_win32.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\core\platform\platform_win32.cpp | 122 | 32 | style | Variable 'high_surrogate' is assigned a value that is never used. | unreadVariable |
| src\tui\core\screen.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\input\line_editor.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\input\line_editor.cpp | 180 | 21 | style | Consider using std::accumulate algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\render\chat_renderer.cpp | 275 | 27 | performance | Ineffective call of function 'substr' because a prefix of the string is assigned to itself. Use replace() instead. | uselessCallsSubstr |
| src\tui\render\markdown_renderer.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\render\markdown_renderer.cpp | 40 | 20 | style | Condition 'cells.empty()' is always false | knownConditionTrueFalse |
| src\tui\render\markdown_renderer.cpp | 265 | 46 | style | Consider using std::accumulate algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\render\markdown_renderer.cpp | 510 | 5 | style | Consider using std::all_of or std::none_of algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\render\markdown_renderer.cpp | 541 | 0 | style | Consider using std::any_of algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\utils\utf8_utils.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\utils\utf8_utils.cpp | 62 | 15 | style | Consider using std::accumulate algorithm instead of a raw loop. | useStlAlgorithm |
| src\tui\widgets\command_panel.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\widgets\file_search_panel.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\widgets\select_panel.cpp | 0 | 0 | information | Limiting analysis of branches. Use --check-level=exhaustive to analyze all branches. | normalCheckLevelMaxBranches |
| src\tui\widgets\status_bar.cpp | 139 | 13 | style | The scope of the variable 'secs' can be reduced. | variableScope |
| src\agent\api\remote\http_client.cpp | 190 | 10 | style | The function 'is_cancelled' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 81 | 10 | style | The function 'set_is_enabled' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 82 | 10 | style | The function 'set_is_hidden' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 83 | 10 | style | The function 'set_user_invocable' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 84 | 10 | style | The function 'set_disable_model_invocation' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 86 | 10 | style | The function 'set_source' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 87 | 10 | style | The function 'set_loaded_from' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 88 | 10 | style | The function 'set_version' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 89 | 10 | style | The function 'set_immediate' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 90 | 10 | style | The function 'set_sensitive' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 91 | 10 | style | The function 'set_when_to_use' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 132 | 10 | style | The function 'set_prompt_generator' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 142 | 10 | style | The function 'set_progress_message' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 143 | 24 | style | The function 'progress_message' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 145 | 10 | style | The function 'set_content_length' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 146 | 12 | style | The function 'content_length' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 148 | 10 | style | The function 'set_allowed_tools' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 149 | 37 | style | The function 'allowed_tools' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 151 | 10 | style | The function 'set_arg_names' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 152 | 37 | style | The function 'arg_names' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 154 | 10 | style | The function 'set_model' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 155 | 39 | style | The function 'model' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 157 | 10 | style | The function 'set_context_type' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 158 | 39 | style | The function 'context_type' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 195 | 10 | style | The function 'set_supports_non_interactive' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 196 | 10 | style | The function 'supports_non_interactive' is never used. | unusedFunction |
| src\agent\command\inclaude\command.h | 204 | 39 | style | The function 'make_prompt_command' is never used. | unusedFunction |
| src\agent\command\source\registry.cpp | 23 | 60 | style | The function 'get_enabled_commands' is never used. | unusedFunction |
| src\agent\command\source\registry.cpp | 43 | 60 | style | The function 'get_model_invocable_commands' is never used. | unusedFunction |
| src\agent\command\source\registry.cpp | 55 | 60 | style | The function 'get_by_type' is never used. | unusedFunction |
| src\agent\command\source\registry.cpp | 65 | 60 | style | The function 'get_by_source' is never used. | unusedFunction |
| src\tui\render\markdown_renderer.cpp | 626 | 13 | style | The function 'render_markdown' is never used. | unusedFunction |
| nofile | 0 | 0 | information | Active checkers: 173/975 (use --checkers-report=<filename> to see details) | checkersReport |

/**
 * @file cli_args.cpp
 * @brief CLI 参数解析实现
 * @details 解析 --provider/--model/--api-key 等命令行参数
 * @version 1.0.0
 * @date 2026-07
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include "agent/model/provider_preset.h"
#include "app/config/app_config.h"
#include "app/config/cli_args.h"
#include "core/config/config_manager.h"

namespace agent {

void parse_cli_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto& cfg = ConfigManager::instance();

        if (arg == "--simple-io") {
            cfg.set(keys::SIMPLE_IO, true);
        } else if (arg == "--no-color") {
            cfg.set(keys::NO_COLOR, true);
        } else if (arg == "--verbose") {
            cfg.set(keys::VERBOSE, true);
        } else if (arg == "--prompt" && i + 1 < argc) {
            cfg.set(keys::PROMPT, std::string(argv[++i]));
        } else if (arg == "--provider" && i + 1 < argc) {
            std::string name = argv[++i];
            if (!find_preset(name)) {
                std::cerr << "Unknown provider: " << name << "\n"
                          << "Available providers:";
                for (auto p : list_preset_names())
                    std::cerr << " " << p;
                std::cerr << "\n";
                exit(1);
            }
            cfg.set(keys::PROVIDER, std::string(name));
        } else if (arg == "--remote" && i + 1 < argc) {
            cfg.set(keys::REMOTE_URL, std::string(argv[++i]));
        } else if (arg == "--model" && i + 1 < argc) {
            cfg.set(keys::MODEL_NAME, std::string(argv[++i]));
        } else if (arg == "--api-key" && i + 1 < argc) {
            cfg.set(keys::API_KEY, std::string(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            try {
                cfg.set(keys::TIMEOUT_MS, std::stoi(argv[++i]));
            } catch (...) {
                std::cerr << "Invalid timeout value\n";
            }
        } else if (arg == "--system-prompt" && i + 1 < argc) {
            cfg.set(keys::SYSTEM_PROMPT, std::string(argv[++i]));
        } else if (arg == "--config" && i + 1 < argc) {
            // --config 已在 run 中提前处理，此处跳过
            ++i;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: workx [options]\n"
                      << "\nOptions:\n"
                      << "  --provider <name>       API provider: openai, anthropic, deepseek,\n"
                      << "                          groq, together, openai-compatible\n"
                      << "  --remote <url>          Remote API base URL\n"
                      << "  --model <name>          Model name\n"
                      << "  --api-key <key>         API key\n"
                      << "  --timeout <ms>          HTTP timeout in ms (default: 30000)\n"
                      << "  --system-prompt <text>  Set system prompt\n"
                      << "  --config <path>         Load config from JSON file\n"
                      << "  --simple-io             Use simple I/O mode (getline)\n"
                      << "  --no-color              Disable colored output\n"
                      << "  --verbose               Show verbose startup debug info\n"
                      << "  --prompt <str>          Set prompt string (default: \"> \")\n"
                      << "  --help, -h              Show this help\n"
                      << "\nWhen --provider is specified, --remote and --model default to\n"
                      << "the provider's preset values but can still be overridden.\n"
                      << "\nEnvironment variables:\n"
                      << "  WORKX_API_KEY           API key (alternative to --api-key)\n"
                      << "  WORKX_BASE_URL          Base URL (alternative to --remote)\n"
                      << "  WORKX_MODEL             Model name (alternative to --model)\n"
                      << "  WORKX_TIMEOUT           Timeout in ms\n"
                      << "  WORKX_NO_COLOR          Set any value to disable color\n"
                      << "\nConfig file:\n"
                      << "  " << default_config_path().string() << "\n"
                      << "\nConfig file example:\n"
                      << "  {\n"
                      << "    \"backend.provider\": \"deepseek\",\n"
                      << "    \"backend.api_key\": \"sk-your-key-here\"\n"
                      << "  }\n"
                      << "  (provider auto-fills remote URL and model)\n";
            exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            exit(1);
        }
    }
}

} // namespace agent

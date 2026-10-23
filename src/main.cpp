#include <csignal>
#include <string>
#include "bot.hpp"
#include "logger.hpp"

static Bot* g_bot = nullptr;

void signal_handler(int sig) {
    LOG_INFO("\n[Main] Caught signal %d, shutting down cleanly...", sig);
    if (g_bot) {
        g_bot->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string config_path = "config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    LOG_INFO("===========================================");
    LOG_INFO("   TeamTalk Native Media Daemon (C++)      ");
    LOG_INFO("   Author: Ivan (Ivankolp)                 ");
    LOG_INFO("===========================================");

    BotConfig cfg = BotConfig::load_from_file(config_path);
    Bot bot(cfg);
    g_bot = &bot;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!bot.init()) {
        LOG_ERR("[Main] Failed to initialize bot!");
        return 1;
    }

    bot.run();

    LOG_INFO("[Main] Bot exited cleanly.");
    return 0;
}

#pragma once
#include <string>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include <nlohmann/json.hpp>

struct BotConfig {
    std::string host = "127.0.0.1";
    int tcp_port = 16815;
    int udp_port = 16815;
    bool encrypted = false;
    
    std::string username = "bot";
    std::string password = "";
    std::string nickname = "TTMediaBot-Native";
    std::string status_message = "C++ Native Audio Bot";
    
    std::string channel_path = "/";
    std::string channel_password = "";
    
    int default_volume = 100;
    std::string bridge_url = "http://127.0.0.1:4417";
    std::vector<std::string> admin_users;
    bool channel_messages = true;
    
    static BotConfig load_from_file(const std::string& path) {
        BotConfig cfg;
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_ERR("[Config] Warning: Could not open config file %s, using defaults.", path.c_str());
            return cfg;
        }
        try {
            nlohmann::json j;
            file >> j;
            if (j.contains("host")) cfg.host = j["host"].get<std::string>();
            if (j.contains("tcp_port")) cfg.tcp_port = j["tcp_port"].get<int>();
            if (j.contains("udp_port")) cfg.udp_port = j["udp_port"].get<int>();
            if (j.contains("encrypted")) cfg.encrypted = j["encrypted"].get<bool>();
            if (j.contains("username")) cfg.username = j["username"].get<std::string>();
            if (j.contains("password")) cfg.password = j["password"].get<std::string>();
            if (j.contains("nickname")) cfg.nickname = j["nickname"].get<std::string>();
            if (j.contains("status_message")) cfg.status_message = j["status_message"].get<std::string>();
            if (j.contains("channel_path")) cfg.channel_path = j["channel_path"].get<std::string>();
            if (j.contains("channel_password")) cfg.channel_password = j["channel_password"].get<std::string>();
            if (j.contains("default_volume")) cfg.default_volume = j["default_volume"].get<int>();
            if (j.contains("bridge_url")) cfg.bridge_url = j["bridge_url"].get<std::string>();
            if (j.contains("channel_messages")) cfg.channel_messages = j["channel_messages"].get<bool>();
            if (j.contains("admin_users") && j["admin_users"].is_array()) {
                for (const auto& u : j["admin_users"]) {
                    cfg.admin_users.push_back(u.get<std::string>());
                }
            }
        } catch (const std::exception& e) {
            LOG_ERR("[Config] Parse error in %s: %s", path.c_str(), e.what());
        }
        return cfg;
    }
};

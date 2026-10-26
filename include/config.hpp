#pragma once
#include <string>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include <nlohmann/json.hpp>

struct BotConfig {
    // General
    std::string language = "ru";
    bool channel_messages = false;
    std::string db_path = "bot.db";
    std::vector<std::string> blocked_commands;
    std::string time_format = "%H:%M";
    std::vector<std::string> start_commands;

    // Player
    int default_volume = 25;
    int max_volume = 100;
    int seek_step = 10;
    bool volume_fading = false;

    // TeamTalk Server
    std::string host = "127.0.0.1";
    int tcp_port = 16815;
    int udp_port = 16815;
    bool encrypted = false;
    std::string nickname = "Музыкальный Бот";
    std::string status_message = "Отправьте h для справки";
    std::string gender = "n"; // 'm', 'f', 'n'
    std::string username = "bot_native";
    std::string password = "musicbotpassword123";
    std::string channel_path = "/";
    std::string channel_password = "";
    int reconnection_attempts = -1;
    int reconnection_timeout = 5;

    // Users
    std::vector<std::string> admin_users;
    std::vector<std::string> banned_users;

    // Services
    std::string default_service = "yt";
    std::string bridge_url = "http://127.0.0.1:4417";
    int search_results_count = 1;

    // File path where config was loaded from
    std::string loaded_file_path = "config.json";

    static BotConfig load_from_file(const std::string& path) {
        BotConfig cfg;
        cfg.loaded_file_path = path;
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_ERR("[Config] Warning: Could not open config file %s, using defaults.", path.c_str());
            return cfg;
        }
        try {
            nlohmann::json j;
            file >> j;

            // 1. Nested TTMediaBot style:
            if (j.contains("general") && j["general"].is_object()) {
                auto& g = j["general"];
                if (g.contains("language")) cfg.language = g["language"].get<std::string>();
                if (g.contains("send_channel_messages")) cfg.channel_messages = g["send_channel_messages"].get<bool>();
                if (g.contains("cache_file_name")) cfg.db_path = g["cache_file_name"].get<std::string>();
                if (g.contains("db_file_name")) cfg.db_path = g["db_file_name"].get<std::string>();
                if (g.contains("status")) cfg.status_message = g["status"].get<std::string>();
                if (g.contains("time_format")) cfg.time_format = g["time_format"].get<std::string>();
                if (g.contains("start_commands") && g["start_commands"].is_array()) {
                    for (const auto& sc : g["start_commands"]) cfg.start_commands.push_back(sc.get<std::string>());
                }
                if (g.contains("blocked_commands") && g["blocked_commands"].is_array()) {
                    for (const auto& c : g["blocked_commands"]) cfg.blocked_commands.push_back(c.get<std::string>());
                }
            }

            if (j.contains("player") && j["player"].is_object()) {
                auto& p = j["player"];
                if (p.contains("default_volume")) cfg.default_volume = p["default_volume"].get<int>();
                if (p.contains("max_volume")) cfg.max_volume = p["max_volume"].get<int>();
                if (p.contains("seek_step")) cfg.seek_step = p["seek_step"].get<int>();
                if (p.contains("volume_fading")) cfg.volume_fading = p["volume_fading"].get<bool>();
            }

            if (j.contains("teamtalk") && j["teamtalk"].is_object()) {
                auto& tt = j["teamtalk"];
                if (tt.contains("hostname")) cfg.host = tt["hostname"].get<std::string>();
                if (tt.contains("tcp_port")) cfg.tcp_port = tt["tcp_port"].get<int>();
                if (tt.contains("udp_port")) cfg.udp_port = tt["udp_port"].get<int>();
                if (tt.contains("encrypted")) cfg.encrypted = tt["encrypted"].get<bool>();
                if (tt.contains("nickname")) cfg.nickname = tt["nickname"].get<std::string>();
                if (tt.contains("status")) cfg.status_message = tt["status"].get<std::string>();
                if (tt.contains("gender")) cfg.gender = tt["gender"].get<std::string>();
                if (tt.contains("username")) cfg.username = tt["username"].get<std::string>();
                if (tt.contains("password")) cfg.password = tt["password"].get<std::string>();
                if (tt.contains("channel")) cfg.channel_path = tt["channel"].get<std::string>();
                if (tt.contains("channel_password")) cfg.channel_password = tt["channel_password"].get<std::string>();
                if (tt.contains("reconnection_attempts")) cfg.reconnection_attempts = tt["reconnection_attempts"].get<int>();
                if (tt.contains("reconnection_timeout")) cfg.reconnection_timeout = tt["reconnection_timeout"].get<int>();

                if (tt.contains("users") && tt["users"].is_object()) {
                    auto& u = tt["users"];
                    if (u.contains("admins") && u["admins"].is_array()) {
                        for (const auto& a : u["admins"]) cfg.admin_users.push_back(a.get<std::string>());
                    }
                    if (u.contains("banned_users") && u["banned_users"].is_array()) {
                        for (const auto& b : u["banned_users"]) cfg.banned_users.push_back(b.get<std::string>());
                    }
                }
            }

            if (j.contains("services") && j["services"].is_object()) {
                auto& s = j["services"];
                if (s.contains("default_service")) cfg.default_service = s["default_service"].get<std::string>();
                if (s.contains("bridge_url")) cfg.bridge_url = s["bridge_url"].get<std::string>();
            }

            // 2. Also support flat format overrides if present:
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

        // Force fallback status if empty
        if (cfg.status_message.empty()) {
            cfg.status_message = "Отправьте h для справки";
        }

        return cfg;
    }

    bool save_to_file(const std::string& path = "") const {
        std::string p = path.empty() ? loaded_file_path : path;
        nlohmann::json j;
        j["config_version"] = 0;
        
        j["general"]["language"] = language;
        j["general"]["send_channel_messages"] = channel_messages;
        j["general"]["cache_file_name"] = db_path;
        j["general"]["blocked_commands"] = blocked_commands;
        j["general"]["time_format"] = time_format;
        j["general"]["status"] = status_message;
        j["general"]["start_commands"] = start_commands;

        j["player"]["default_volume"] = default_volume;
        j["player"]["max_volume"] = max_volume;
        j["player"]["seek_step"] = seek_step;
        j["player"]["volume_fading"] = volume_fading;

        j["teamtalk"]["hostname"] = host;
        j["teamtalk"]["tcp_port"] = tcp_port;
        j["teamtalk"]["udp_port"] = udp_port;
        j["teamtalk"]["encrypted"] = encrypted;
        j["teamtalk"]["nickname"] = nickname;
        j["teamtalk"]["status"] = status_message;
        j["teamtalk"]["gender"] = gender;
        j["teamtalk"]["username"] = username;
        j["teamtalk"]["password"] = password;
        j["teamtalk"]["channel"] = channel_path;
        j["teamtalk"]["channel_password"] = channel_password;
        j["teamtalk"]["reconnection_attempts"] = reconnection_attempts;
        j["teamtalk"]["reconnection_timeout"] = reconnection_timeout;
        j["teamtalk"]["users"]["admins"] = admin_users;
        j["teamtalk"]["users"]["banned_users"] = banned_users;

        j["services"]["default_service"] = default_service;
        j["services"]["bridge_url"] = bridge_url;
        j["services"]["yt"]["enabled"] = true;
        j["services"]["yt"]["search_results"] = search_results_count;
        j["services"]["ytm"]["enabled"] = true;
        j["services"]["ytm"]["search_results"] = search_results_count;

        std::ofstream file(p);
        if (!file.is_open()) return false;
        file << j.dump(4);
        return true;
    }
};

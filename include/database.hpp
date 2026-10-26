#pragma once
#include <string>
#include <vector>
#include <mutex>
#include "types.hpp"

struct sqlite3;

class Database {
public:
    explicit Database(const std::string& db_path);
    ~Database();

    bool init();

    // Key-value settings
    std::string get_setting(const std::string& key, const std::string& default_val = "");
    bool set_setting(const std::string& key, const std::string& val);

    int get_int(const std::string& key, int default_val);
    void set_int(const std::string& key, int val);

    float get_float(const std::string& key, float default_val);
    void set_float(const std::string& key, float val);

    // High level settings
    int get_volume(int default_val = 25);
    void set_volume(int vol);

    PlaybackMode get_mode(PlaybackMode default_val = PlaybackMode::Normal);
    void set_mode(PlaybackMode mode);

    float get_speed(float default_val = 1.0f);
    void set_speed(float speed);

    std::string get_status(const std::string& default_val = "Отправьте h для справки");
    void set_status(const std::string& status);

    // Favorites
    bool add_favorite(const Track& track);
    bool remove_favorite(int index);
    std::vector<Track> get_favorites();
    void clear_favorites();

    // Recents
    void add_recent(const Track& track);
    std::vector<Track> get_recents(int limit = 30);
    void clear_recents();

    // Queue persistence
    void save_queue(const std::vector<Track>& queue);
    std::vector<Track> load_queue();
    void clear_queue();

    // Blocked commands
    bool is_command_blocked(const std::string& cmd);
    void block_command(const std::string& cmd);
    void unblock_command(const std::string& cmd);
    std::vector<std::string> get_blocked_commands();

    // Banned users
    bool is_user_banned(const std::string& username);
    void ban_user(const std::string& username);
    void unban_user(const std::string& username);
    std::vector<std::string> get_banned_users();

    // Admin users
    bool is_admin_user(const std::string& username);
    void add_admin_user(const std::string& username);
    void remove_admin_user(const std::string& username);
    std::vector<std::string> get_admin_users();

private:
    std::string db_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

#include "database.hpp"
#include "logger.hpp"
#include <sqlite3.h>
#include <sstream>
#include <algorithm>
#include <filesystem>

Database::Database(const std::string& db_path)
    : db_path_(db_path) {}

Database::~Database() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::filesystem::path p(db_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {}

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERR("[Database] Cannot open database %s: %s", db_path_.c_str(), sqlite3_errmsg(db_));
        return false;
    }

    const char* schema =
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  val TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS favorites ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT,"
        "  url TEXT UNIQUE,"
        "  duration INTEGER,"
        "  uploader TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS recents ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT,"
        "  url TEXT,"
        "  duration INTEGER,"
        "  uploader TEXT,"
        "  played_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS queue ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  position INTEGER,"
        "  title TEXT,"
        "  url TEXT,"
        "  duration INTEGER,"
        "  uploader TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS blocked_commands ("
        "  cmd TEXT PRIMARY KEY"
        ");"
        "CREATE TABLE IF NOT EXISTS banned_users ("
        "  username TEXT PRIMARY KEY"
        ");"
        "CREATE TABLE IF NOT EXISTS admin_users ("
        "  username TEXT PRIMARY KEY"
        ");";

    char* err_msg = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_ERR("[Database] Schema creation failed: %s", err_msg ? err_msg : "unknown");
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    // Performance pragmas: WAL mode for concurrent reads, reduced sync for speed
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-8000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);

    LOG_INFO("[Database] SQLite database initialized successfully: %s", db_path_.c_str());
    return true;
}

std::string Database::get_setting(const std::string& key, const std::string& default_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return default_val;

    const char* sql = "SELECT val FROM settings WHERE key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    std::string result = default_val;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* val = sqlite3_column_text(stmt, 0);
            if (val) {
                result = reinterpret_cast<const char*>(val);
            }
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

bool Database::set_setting(const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR REPLACE INTO settings (key, val) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, val.c_str(), -1, SQLITE_STATIC);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    return ok;
}

int Database::get_int(const std::string& key, int default_val) {
    std::string s = get_setting(key, "");
    if (s.empty()) return default_val;
    try {
        return std::stoi(s);
    } catch (...) {
        return default_val;
    }
}

void Database::set_int(const std::string& key, int val) {
    set_setting(key, std::to_string(val));
}

float Database::get_float(const std::string& key, float default_val) {
    std::string s = get_setting(key, "");
    if (s.empty()) return default_val;
    try {
        return std::stof(s);
    } catch (...) {
        return default_val;
    }
}

void Database::set_float(const std::string& key, float val) {
    set_setting(key, std::to_string(val));
}

int Database::get_volume(int default_val) {
    int v = get_int("volume", default_val);
    return std::clamp(v, 0, 100);
}

void Database::set_volume(int vol) {
    set_int("volume", std::clamp(vol, 0, 100));
}

PlaybackMode Database::get_mode(PlaybackMode default_val) {
    int m = get_int("mode", static_cast<int>(default_val));
    if (m >= 0 && m <= 3) {
        return static_cast<PlaybackMode>(m);
    }
    return default_val;
}

void Database::set_mode(PlaybackMode mode) {
    set_int("mode", static_cast<int>(mode));
}

float Database::get_speed(float default_val) {
    float s = get_float("speed", default_val);
    return std::clamp(s, 0.5f, 2.0f);
}

void Database::set_speed(float speed) {
    set_float("speed", std::clamp(speed, 0.5f, 2.0f));
}

std::string Database::get_status(const std::string& default_val) {
    return get_setting("status", default_val);
}

void Database::set_status(const std::string& status) {
    set_setting("status", status);
}

bool Database::add_favorite(const Track& track) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR IGNORE INTO favorites (title, url, duration, uploader) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, track.title.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, track.webpage_url.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, track.duration_sec);
        sqlite3_bind_text(stmt, 4, track.uploader.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            ok = (sqlite3_changes(db_) > 0);
        }
        sqlite3_finalize(stmt);
    }
    return ok;
}

bool Database::remove_favorite(int index) {
    auto favs = get_favorites();
    if (index < 0 || index >= static_cast<int>(favs.size())) {
        return false;
    }
    std::string target_url = favs[index].webpage_url;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "DELETE FROM favorites WHERE url = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, target_url.c_str(), -1, SQLITE_STATIC);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    return ok;
}

std::vector<Track> Database::get_favorites() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Track> tracks;
    if (!db_) return tracks;

    const char* sql = "SELECT title, url, duration, uploader FROM favorites ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Track t;
            const unsigned char* title = sqlite3_column_text(stmt, 0);
            const unsigned char* url = sqlite3_column_text(stmt, 1);
            int dur = sqlite3_column_int(stmt, 2);
            const unsigned char* up = sqlite3_column_text(stmt, 3);

            if (title) t.title = reinterpret_cast<const char*>(title);
            if (url) {
                t.webpage_url = reinterpret_cast<const char*>(url);
                t.id = t.webpage_url;
            }
            t.duration_sec = dur;
            if (up) t.uploader = reinterpret_cast<const char*>(up);

            tracks.push_back(t);
        }
        sqlite3_finalize(stmt);
    }
    return tracks;
}

void Database::clear_favorites() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    sqlite3_exec(db_, "DELETE FROM favorites;", nullptr, nullptr, nullptr);
}

void Database::add_recent(const Track& track) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;

    const char* sql = "INSERT INTO recents (title, url, duration, uploader) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, track.title.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, track.webpage_url.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, track.duration_sec);
        sqlite3_bind_text(stmt, 4, track.uploader.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Keep only last 100 recents
    sqlite3_exec(db_, "DELETE FROM recents WHERE id NOT IN (SELECT id FROM recents ORDER BY id DESC LIMIT 100);", nullptr, nullptr, nullptr);
}

std::vector<Track> Database::get_recents(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Track> tracks;
    if (!db_) return tracks;

    const char* sql = "SELECT title, url, duration, uploader FROM recents ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Track t;
            const unsigned char* title = sqlite3_column_text(stmt, 0);
            const unsigned char* url = sqlite3_column_text(stmt, 1);
            int dur = sqlite3_column_int(stmt, 2);
            const unsigned char* up = sqlite3_column_text(stmt, 3);

            if (title) t.title = reinterpret_cast<const char*>(title);
            if (url) {
                t.webpage_url = reinterpret_cast<const char*>(url);
                t.id = t.webpage_url;
            }
            t.duration_sec = dur;
            if (up) t.uploader = reinterpret_cast<const char*>(up);

            tracks.push_back(t);
        }
        sqlite3_finalize(stmt);
    }
    return tracks;
}

void Database::clear_recents() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    sqlite3_exec(db_, "DELETE FROM recents;", nullptr, nullptr, nullptr);
}

void Database::save_queue(const std::vector<Track>& queue) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;

    sqlite3_exec(db_, "BEGIN TRANSACTION; DELETE FROM queue;", nullptr, nullptr, nullptr);
    const char* sql = "INSERT INTO queue (position, title, url, duration, uploader) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < queue.size(); ++i) {
            sqlite3_bind_int(stmt, 1, static_cast<int>(i));
            sqlite3_bind_text(stmt, 2, queue[i].title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, queue[i].webpage_url.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, queue[i].duration_sec);
            sqlite3_bind_text(stmt, 5, queue[i].uploader.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<Track> Database::load_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Track> tracks;
    if (!db_) return tracks;

    const char* sql = "SELECT title, url, duration, uploader FROM queue ORDER BY position ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Track t;
            const unsigned char* title = sqlite3_column_text(stmt, 0);
            const unsigned char* url = sqlite3_column_text(stmt, 1);
            int dur = sqlite3_column_int(stmt, 2);
            const unsigned char* up = sqlite3_column_text(stmt, 3);

            if (title) t.title = reinterpret_cast<const char*>(title);
            if (url) {
                t.webpage_url = reinterpret_cast<const char*>(url);
                t.id = t.webpage_url;
            }
            t.duration_sec = dur;
            if (up) t.uploader = reinterpret_cast<const char*>(up);

            tracks.push_back(t);
        }
        sqlite3_finalize(stmt);
    }
    return tracks;
}

void Database::clear_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    sqlite3_exec(db_, "DELETE FROM queue;", nullptr, nullptr, nullptr);
}

bool Database::is_command_blocked(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM blocked_commands WHERE cmd = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool blocked = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, cmd.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            blocked = true;
        }
        sqlite3_finalize(stmt);
    }
    return blocked;
}

void Database::block_command(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "INSERT OR IGNORE INTO blocked_commands (cmd) VALUES (?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, cmd.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::unblock_command(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "DELETE FROM blocked_commands WHERE cmd = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, cmd.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::string> Database::get_blocked_commands() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> res;
    if (!db_) return res;
    const char* sql = "SELECT cmd FROM blocked_commands ORDER BY cmd ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) res.push_back(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
    }
    return res;
}

bool Database::is_user_banned(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM banned_users WHERE username = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool banned = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            banned = true;
        }
        sqlite3_finalize(stmt);
    }
    return banned;
}

void Database::ban_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "INSERT OR IGNORE INTO banned_users (username) VALUES (?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::unban_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "DELETE FROM banned_users WHERE username = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::string> Database::get_banned_users() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> res;
    if (!db_) return res;
    const char* sql = "SELECT username FROM banned_users ORDER BY username ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) res.push_back(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
    }
    return res;
}

bool Database::is_admin_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM admin_users WHERE username = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool is_admin = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            is_admin = true;
        }
        sqlite3_finalize(stmt);
    }
    return is_admin;
}

void Database::add_admin_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "INSERT OR IGNORE INTO admin_users (username) VALUES (?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::remove_admin_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    const char* sql = "DELETE FROM admin_users WHERE username = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::string> Database::get_admin_users() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> res;
    if (!db_) return res;
    const char* sql = "SELECT username FROM admin_users ORDER BY username ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) res.push_back(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
    }
    return res;
}

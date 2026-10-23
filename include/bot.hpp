#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include "TeamTalk.h"
#include "config.hpp"
#include "youtube_resolver.hpp"
#include "audio_streamer.hpp"
#include "playlist_manager.hpp"

class Bot {
public:
    explicit Bot(BotConfig config);
    ~Bot();

    bool init();
    void run();
    void stop();

private:
    BotConfig config_;
    TTInstance* tt_{nullptr};
    std::atomic<bool> running_{false};

    std::unique_ptr<YouTubeResolver> resolver_;
    std::unique_ptr<AudioStreamer> streamer_;
    std::unique_ptr<PlaylistManager> playlist_;

    std::unordered_map<int, std::vector<Track>> pending_searches_;
    int search_limit_{5};
    bool voice_transmission_enabled_{true};

    void connect_and_login();
    void process_event(const TTMessage& msg);
    void handle_text_message(const TextMessage& msg);
    
    void send_message(int to_user_id, int channel_id, const std::string& text);
    void reply(const TextMessage& original, const std::string& text);
    bool is_admin(const std::string& username) const;

    // Track playback helper
    void play_track(const Track& track, const TextMessage* reply_to = nullptr);
    void on_track_finished();

    // User commands
    void cmd_play(const TextMessage& msg, const std::string& arg);
    void cmd_play_url(const TextMessage& msg, const std::string& arg);
    void cmd_stop(const TextMessage& msg);
    void cmd_back(const TextMessage& msg);
    void cmd_next(const TextMessage& msg, const std::string& arg);
    void cmd_current(const TextMessage& msg, const std::string& arg);
    void cmd_seek(const TextMessage& msg, int delta_sec);
    void cmd_volume(const TextMessage& msg, const std::string& arg);
    void cmd_speed(const TextMessage& msg, const std::string& arg);
    void cmd_mode(const TextMessage& msg, const std::string& arg);
    void cmd_favorites(const TextMessage& msg, const std::string& arg);
    void cmd_recents(const TextMessage& msg);
    void cmd_get_link(const TextMessage& msg);
    void cmd_queue_add(const TextMessage& msg, const std::string& arg);
    void cmd_queue_list(const TextMessage& msg);
    void cmd_queue_remove(const TextMessage& msg, const std::string& arg);
    void cmd_queue_clear(const TextMessage& msg);
    void cmd_queue_skip(const TextMessage& msg);
    void cmd_search(const TextMessage& msg, const std::string& arg);
    void cmd_select_search(const TextMessage& msg, const std::string& arg);
    void cmd_select_search_count(const TextMessage& msg, const std::string& arg);
    void cmd_join_channel(const TextMessage& msg, const std::string& arg);
    void cmd_help(const TextMessage& msg, const std::string& arg);
    void cmd_about(const TextMessage& msg);

    // Admin commands
    void cmd_change_nick(const TextMessage& msg, const std::string& arg);
    void cmd_change_status(const TextMessage& msg, const std::string& arg);
    void cmd_voice_toggle(const TextMessage& msg, const std::string& arg);
    void cmd_channel_msg_toggle(const TextMessage& msg, const std::string& arg);
    void cmd_get_channel_id(const TextMessage& msg);
};

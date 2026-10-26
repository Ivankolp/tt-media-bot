#pragma once
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <random>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <chrono>
#include "types.hpp"

class PlaylistManager {
public:
    explicit PlaylistManager(std::string data_dir = ".");
    ~PlaylistManager();

    // Track list / Playlist (from search, album, playlist)
    void set_track_list(const std::vector<Track>& tracks, int start_index = 0);
    void append_to_track_list(const std::vector<Track>& tracks);
    size_t append_unique(const std::vector<Track>& new_tracks);
    bool has_track_id(const std::string& id) const;
    std::vector<Track> get_track_list() const;
    int get_track_index() const;
    bool has_tracks() const;

    // Explicit User Queue (qa, ql, qr, qc)
    void add_to_queue(const Track& track);
    bool pop_from_queue(Track& out_track);
    void clear_queue();
    bool remove_from_queue(size_t index);
    std::vector<Track> get_queue() const;

    // Playback navigation (n / b)
    bool pop_next(Track& out_track);
    bool pop_prev(Track& out_track);
    bool jump_to_index(int index, Track& out_track);

    // History (r command)
    void add_recent(const Track& track);
    std::vector<Track> get_recents() const;

    // Favorites (f command)
    bool add_favorite(const Track& track);
    bool remove_favorite(size_t index);
    std::vector<Track> get_favorites() const;

    // Modes
    void set_mode(PlaybackMode mode);
    PlaybackMode get_mode() const;
    std::string get_mode_name() const;

    // Clear everything
    void clear_all();

    // Current track
    void set_current_track(const Track& track);
    Track get_current_track() const;

private:
    std::string data_dir_;
    mutable std::mutex mtx_;

    std::vector<Track> track_list_;
    int track_index_{-1};

    std::deque<Track> queue_;           // Explicit user queue (qa)
    std::deque<Track> history_;         // Recent played tracks
    std::deque<Track> forward_history_; // For stepping forward after stepping back
    std::vector<Track> favorites_;
    PlaybackMode mode_{PlaybackMode::Normal};
    Track current_playing_;

    // O(1) dedup set for append_unique
    std::unordered_set<std::string> known_ids_;

    // Debounced save
    std::atomic<bool> save_pending_{false};
    std::thread save_thread_;

    void load_favorites();
    void save_favorites();
    void schedule_save();
    void debounced_save_worker();
};
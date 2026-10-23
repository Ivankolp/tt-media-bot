#pragma once
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <random>
#include "types.hpp"

class PlaylistManager {
public:
    explicit PlaylistManager(std::string data_dir = ".");
    ~PlaylistManager();

    void add_to_queue(const Track& track);
    bool pop_next(Track& out_track);
    bool pop_prev(Track& out_track);
    void clear_queue();
    bool remove_from_queue(size_t index);
    std::vector<Track> get_queue() const;

    void add_recent(const Track& track);
    std::vector<Track> get_recents() const;

    bool add_favorite(const Track& track);
    bool remove_favorite(size_t index);
    std::vector<Track> get_favorites() const;

    void set_mode(PlaybackMode mode);
    PlaybackMode get_mode() const;
    std::string get_mode_name() const;

private:
    std::string data_dir_;
    mutable std::mutex mtx_;
    std::deque<Track> queue_;
    std::deque<Track> history_;
    std::vector<Track> favorites_;
    PlaybackMode mode_{PlaybackMode::Normal};
    Track current_playing_;

    void load_favorites();
    void save_favorites();
};

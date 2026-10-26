#include "playlist_manager.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

PlaylistManager::PlaylistManager(std::string data_dir)
    : data_dir_(std::move(data_dir)) {
    load_favorites();
}

PlaylistManager::~PlaylistManager() {
    if (save_pending_.exchange(false)) {
        save_favorites();
    }
    if (save_thread_.joinable()) {
        save_thread_.join();
    }
}

void PlaylistManager::set_track_list(const std::vector<Track>& tracks, int start_index) {
    std::lock_guard<std::mutex> lock(mtx_);
    track_list_ = tracks;
    track_index_ = start_index;
    forward_history_.clear();
    // Rebuild known_ids from new track list + history
    known_ids_.clear();
    for (const auto& t : history_) {
        if (!t.id.empty()) known_ids_.insert(t.id);
    }
    for (const auto& t : track_list_) {
        if (!t.id.empty()) known_ids_.insert(t.id);
    }
    if (start_index >= 0 && start_index < static_cast<int>(tracks.size())) {
        current_playing_ = tracks[start_index];
    }
}

void PlaylistManager::append_to_track_list(const std::vector<Track>& tracks) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& t : tracks) {
        if (!t.id.empty()) known_ids_.insert(t.id);
    }
    track_list_.insert(track_list_.end(), tracks.begin(), tracks.end());
}

size_t PlaylistManager::append_unique(const std::vector<Track>& new_tracks) {
    std::lock_guard<std::mutex> lock(mtx_);
    size_t added = 0;
    for (const auto& t : new_tracks) {
        if (t.id.empty()) continue;
        if (known_ids_.count(t.id)) continue;
        known_ids_.insert(t.id);
        track_list_.push_back(t);
        added++;
    }
    return added;
}

bool PlaylistManager::has_track_id(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (id.empty()) return false;
    return known_ids_.count(id) > 0;
}

std::vector<Track> PlaylistManager::get_track_list() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return track_list_;
}

int PlaylistManager::get_track_index() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return track_index_;
}

bool PlaylistManager::has_tracks() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return !track_list_.empty();
}

void PlaylistManager::add_to_queue(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(track);
}

bool PlaylistManager::pop_from_queue(Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return false;
    out_track = queue_.front();
    queue_.pop_front();
    current_playing_ = out_track;
    return true;
}

void PlaylistManager::clear_queue() {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
}

bool PlaylistManager::remove_from_queue(size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= queue_.size()) return false;
    queue_.erase(queue_.begin() + static_cast<long>(index));
    return true;
}

std::vector<Track> PlaylistManager::get_queue() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<Track>(queue_.begin(), queue_.end());
}

bool PlaylistManager::pop_next(Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 1. RepeatOne mode
    if (mode_ == PlaybackMode::RepeatOne && !current_playing_.title.empty()) {
        out_track = current_playing_;
        return true;
    }

    // 2. Explicit user queue (qa) has highest priority
    if (!queue_.empty()) {
        out_track = queue_.front();
        queue_.pop_front();
        current_playing_ = out_track;
        return true;
    }

    // 3. Forward history (if user navigated back with 'b' and now going forward)
    if (!forward_history_.empty()) {
        out_track = forward_history_.front();
        forward_history_.pop_front();
        current_playing_ = out_track;
        return true;
    }

    // 4. Track list (from search or album)
    if (!track_list_.empty()) {
        if (mode_ == PlaybackMode::Random && track_list_.size() > 1) {
            static std::random_device rd;
            static std::mt19937 gen(rd());
            std::uniform_int_distribution<size_t> dis(0, track_list_.size() - 1);
            size_t idx = dis(gen);
            track_index_ = static_cast<int>(idx);
            out_track = track_list_[track_index_];
            current_playing_ = out_track;
            return true;
        }

        int next_idx = track_index_ + 1;
        if (next_idx < static_cast<int>(track_list_.size())) {
            track_index_ = next_idx;
            out_track = track_list_[track_index_];
            current_playing_ = out_track;
            return true;
        } else if (mode_ == PlaybackMode::RepeatAll && !track_list_.empty()) {
            track_index_ = 0;
            out_track = track_list_[0];
            current_playing_ = out_track;
            return true;
        }
    }

    return false;
}

bool PlaylistManager::pop_prev(Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 1. RepeatOne mode
    if (mode_ == PlaybackMode::RepeatOne && !current_playing_.title.empty()) {
        out_track = current_playing_;
        return true;
    }

    // 2. Track list navigation
    if (!track_list_.empty()) {
        if (track_index_ > 0) {
            if (!current_playing_.title.empty()) {
                forward_history_.push_front(current_playing_);
            }
            track_index_--;
            out_track = track_list_[track_index_];
            current_playing_ = out_track;
            return true;
        } else if (mode_ == PlaybackMode::RepeatAll && !track_list_.empty()) {
            if (!current_playing_.title.empty()) {
                forward_history_.push_front(current_playing_);
            }
            track_index_ = static_cast<int>(track_list_.size()) - 1;
            out_track = track_list_[track_index_];
            current_playing_ = out_track;
            return true;
        }
    }

    // 3. Fallback to play history
    if (!history_.empty() && history_.back().title == current_playing_.title) {
        history_.pop_back();
    }
    if (!history_.empty()) {
        if (!current_playing_.title.empty()) {
            forward_history_.push_front(current_playing_);
        }
        out_track = history_.back();
        history_.pop_back();
        current_playing_ = out_track;
        return true;
    }

    return false;
}

bool PlaylistManager::jump_to_index(int index, Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= 0 && index < static_cast<int>(track_list_.size())) {
        track_index_ = index;
        out_track = track_list_[track_index_];
        current_playing_ = out_track;
        forward_history_.clear();
        return true;
    }
    return false;
}

void PlaylistManager::add_recent(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (track.title.empty()) return;
    if (!history_.empty() && history_.back().title == track.title) return;
    history_.push_back(track);
    if (history_.size() > 50) {
        history_.pop_front();
    }
}

std::vector<Track> PlaylistManager::get_recents() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<Track>(history_.rbegin(), history_.rend());
}

void PlaylistManager::schedule_save() {
    bool expected = false;
    if (save_pending_.compare_exchange_strong(expected, true)) {
        if (save_thread_.joinable()) {
            save_thread_.join();
        }
        save_thread_ = std::thread(&PlaylistManager::debounced_save_worker, this);
    }
}

void PlaylistManager::debounced_save_worker() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    save_pending_ = false;
    save_favorites();
}

bool PlaylistManager::add_favorite(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& f : favorites_) {
        if (f.id == track.id) return false;
    }
    favorites_.push_back(track);
    schedule_save();
    return true;
}

bool PlaylistManager::remove_favorite(size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= favorites_.size()) return false;
    favorites_.erase(favorites_.begin() + static_cast<long>(index));
    schedule_save();
    return true;
}

std::vector<Track> PlaylistManager::get_favorites() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return favorites_;
}

void PlaylistManager::set_mode(PlaybackMode mode) {
    std::lock_guard<std::mutex> lock(mtx_);
    mode_ = mode;
}

PlaybackMode PlaylistManager::get_mode() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return mode_;
}

std::string PlaylistManager::get_mode_name() const {
    std::lock_guard<std::mutex> lock(mtx_);
    switch (mode_) {
        case PlaybackMode::Normal: return "Normal";
        case PlaybackMode::RepeatOne: return "Repeat One";
        case PlaybackMode::RepeatAll: return "Repeat All";
        case PlaybackMode::Random: return "Random";
    }
    return "Unknown";
}

void PlaylistManager::clear_all() {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
    track_list_.clear();
    track_index_ = -1;
    forward_history_.clear();
    current_playing_ = Track();
}

void PlaylistManager::set_current_track(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    current_playing_ = track;
}

Track PlaylistManager::get_current_track() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return current_playing_;
}

void PlaylistManager::load_favorites() {
    std::string path = data_dir_ + "/favorites.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.is_array()) {
            favorites_.clear();
            for (const auto& item : j) {
                Track t;
                t.id = item.value("id", "");
                t.title = item.value("title", "");
                t.uploader = item.value("uploader", "");
                t.webpage_url = item.value("webpage_url", "");
                t.stream_url = item.value("stream_url", "");
                t.duration_sec = item.value("duration_sec", 0);
                favorites_.push_back(t);
            }
        }
    } catch (...) {}
}

void PlaylistManager::save_favorites() {
    std::vector<Track> favs_copy;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        favs_copy = favorites_;
    }
    std::string path = data_dir_ + "/favorites.json";
    std::ofstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& t : favs_copy) {
            nlohmann::json item;
            item["id"] = t.id;
            item["title"] = t.title;
            item["uploader"] = t.uploader;
            item["webpage_url"] = t.webpage_url;
            item["stream_url"] = t.stream_url;
            item["duration_sec"] = t.duration_sec;
            j.push_back(item);
        }
        f << j.dump(2);
    } catch (...) {}
}
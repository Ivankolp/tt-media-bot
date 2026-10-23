#include "playlist_manager.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

PlaylistManager::PlaylistManager(std::string data_dir)
    : data_dir_(std::move(data_dir)) {
    load_favorites();
}

PlaylistManager::~PlaylistManager() {
    save_favorites();
}

void PlaylistManager::add_to_queue(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(track);
}

bool PlaylistManager::pop_next(Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (mode_ == PlaybackMode::RepeatOne && !current_playing_.stream_url.empty()) {
        out_track = current_playing_;
        return true;
    }

    if (queue_.empty()) {
        if (mode_ == PlaybackMode::RepeatAll && !history_.empty()) {
            for (const auto& t : history_) {
                queue_.push_back(t);
            }
            history_.clear();
        } else {
            return false;
        }
    }

    if (mode_ == PlaybackMode::Random && queue_.size() > 1) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, queue_.size() - 1);
        size_t idx = dis(gen);
        out_track = queue_[idx];
        queue_.erase(queue_.begin() + idx);
    } else {
        out_track = queue_.front();
        queue_.pop_front();
    }

    current_playing_ = out_track;
    return true;
}

bool PlaylistManager::pop_prev(Track& out_track) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty()) return false;
    out_track = history_.back();
    history_.pop_back();
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
    queue_.erase(queue_.begin() + index);
    return true;
}

std::vector<Track> PlaylistManager::get_queue() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<Track>(queue_.begin(), queue_.end());
}

void PlaylistManager::add_recent(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    history_.push_back(track);
    if (history_.size() > 30) {
        history_.pop_front();
    }
}

std::vector<Track> PlaylistManager::get_recents() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<Track>(history_.rbegin(), history_.rend());
}

bool PlaylistManager::add_favorite(const Track& track) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& f : favorites_) {
        if (f.id == track.id) return false;
    }
    favorites_.push_back(track);
    save_favorites();
    return true;
}

bool PlaylistManager::remove_favorite(size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= favorites_.size()) return false;
    favorites_.erase(favorites_.begin() + index);
    save_favorites();
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
    std::string path = data_dir_ + "/favorites.json";
    std::ofstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& t : favorites_) {
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

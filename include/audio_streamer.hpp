#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include "TeamTalk.h"
#include "types.hpp"

class AudioStreamer {
public:
    explicit AudioStreamer(TTInstance* tt);
    ~AudioStreamer();

    bool play(const Track& track, int offset_sec = 0);
    void pause();
    void resume();
    void toggle_pause();
    void stop();
    void seek(int relative_sec);
    void seek_to(int absolute_sec);
    
    void set_volume(int vol_0_100);
    int get_volume() const;

    void set_speed(float spd);
    float get_speed() const;

    bool is_playing() const;
    bool is_paused() const;
    int get_position() const;
    Track get_current_track() const;

    void set_on_finished_callback(std::function<void()> cb);

private:
    TTInstance* tt_;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> volume_{100};
    std::atomic<float> speed_{1.0f};
    std::atomic<int> position_sec_{0};
    std::atomic<int> stream_id_counter_{1};

    Track current_track_;
    mutable std::mutex track_mutex_;

    std::thread worker_thread_;
    std::function<void()> on_finished_;
    pid_t current_pid_{-1};

    void streaming_loop(Track track, int start_offset_sec, int stream_id);
    void kill_process();
};

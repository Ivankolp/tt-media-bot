#include "audio_streamer.hpp"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <chrono>
#include <algorithm>
#include <cstring>

AudioStreamer::AudioStreamer(TTInstance* tt)
    : tt_(tt) {}

AudioStreamer::~AudioStreamer() {
    stop();
}

void AudioStreamer::kill_process() {
    if (current_pid_ > 0) {
        kill(current_pid_, SIGKILL);
        int status = 0;
        waitpid(current_pid_, &status, WNOHANG);
        current_pid_ = -1;
    }
}

bool AudioStreamer::play(const Track& track, int offset_sec) {
    stop();

    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        current_track_ = track;
    }

    stop_requested_ = false;
    is_paused_ = false;
    is_playing_ = true;
    position_sec_ = offset_sec;

    int stream_id = ++stream_id_counter_;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    worker_thread_ = std::thread(&AudioStreamer::streaming_loop, this, track, offset_sec, stream_id);
    return true;
}

void AudioStreamer::pause() {
    is_paused_ = true;
}

void AudioStreamer::resume() {
    is_paused_ = false;
}

void AudioStreamer::toggle_pause() {
    is_paused_ = !is_paused_;
}

void AudioStreamer::stop() {
    stop_requested_ = true;
    kill_process();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    is_playing_ = false;
    is_paused_ = false;
    position_sec_ = 0;

    // Signal end of stream to TeamTalk engine
    if (tt_) {
        TT_InsertAudioBlock(tt_, nullptr);
    }
}

void AudioStreamer::seek(int relative_sec) {
    int new_pos = std::max(0, position_sec_.load() + relative_sec);
    seek_to(new_pos);
}

void AudioStreamer::seek_to(int absolute_sec) {
    Track t;
    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        t = current_track_;
    }
    if (!t.stream_url.empty()) {
        play(t, absolute_sec);
    }
}

void AudioStreamer::set_volume(int vol_0_100) {
    volume_ = std::clamp(vol_0_100, 0, 100);
}

int AudioStreamer::get_volume() const {
    return volume_.load();
}

void AudioStreamer::set_speed(float spd) {
    speed_ = std::clamp(spd, 0.5f, 2.0f);
    // Restart with new speed at current offset
    Track t;
    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        t = current_track_;
    }
    if (!t.stream_url.empty() && is_playing_) {
        play(t, position_sec_.load());
    }
}

float AudioStreamer::get_speed() const {
    return speed_.load();
}

bool AudioStreamer::is_playing() const {
    return is_playing_.load();
}

bool AudioStreamer::is_paused() const {
    return is_paused_.load();
}

int AudioStreamer::get_position() const {
    return position_sec_.load();
}

Track AudioStreamer::get_current_track() const {
    std::lock_guard<std::mutex> lock(track_mutex_);
    return current_track_;
}

void AudioStreamer::set_on_finished_callback(std::function<void()> cb) {
    on_finished_ = std::move(cb);
}

void AudioStreamer::streaming_loop(Track track, int start_offset_sec, int stream_id) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        is_playing_ = false;
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        is_playing_ = false;
        return;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::string offset_str = std::to_string(start_offset_sec);
        std::string speed_filter = "atempo=" + std::to_string(speed_.load());

        std::vector<const char*> args = {
            "ffmpeg",
            "-ss", offset_str.c_str(),
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "5",
            "-i", track.stream_url.c_str(),
            "-filter:a", speed_filter.c_str(),
            "-f", "s16le",
            "-ar", "48000",
            "-ac", "2",
            "-loglevel", "quiet",
            "pipe:1",
            nullptr
        };

        execvp("ffmpeg", const_cast<char* const*>(args.data()));
        _exit(1);
    }

    // Parent process
    close(pipefd[1]);
    int read_fd = pipefd[0];
    current_pid_ = pid;

    // 20ms frame at 48000Hz stereo 16-bit:
    // 48000 * 0.02 = 960 samples per channel.
    // 960 * 2 channels = 1920 int16 samples = 3840 bytes.
    const size_t SAMPLES_PER_FRAME = 960;
    const size_t CHANNELS = 2;
    const size_t BYTES_PER_FRAME = SAMPLES_PER_FRAME * CHANNELS * sizeof(int16_t);

    std::vector<int16_t> audio_buf(SAMPLES_PER_FRAME * CHANNELS);
    char* byte_ptr = reinterpret_cast<char*>(audio_buf.data());

    auto next_frame_time = std::chrono::steady_clock::now();
    uint64_t total_frames_sent = 0;

    while (!stop_requested_.load()) {
        if (is_paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_frame_time = std::chrono::steady_clock::now();
            continue;
        }

        size_t bytes_read = 0;
        while (bytes_read < BYTES_PER_FRAME && !stop_requested_.load()) {
            ssize_t n = read(read_fd, byte_ptr + bytes_read, BYTES_PER_FRAME - bytes_read);
            if (n <= 0) {
                // EOF or error
                break;
            }
            bytes_read += n;
        }

        if (bytes_read < BYTES_PER_FRAME) {
            // Stream ended
            break;
        }

        // Apply volume scaling
        int vol = volume_.load();
        if (vol < 100) {
            float factor = vol / 100.0f;
            for (size_t i = 0; i < audio_buf.size(); ++i) {
                audio_buf[i] = static_cast<int16_t>(audio_buf[i] * factor);
            }
        }

        // Submit to TeamTalk
        AudioBlock block{};
        block.nStreamID = stream_id;
        block.nSampleRate = 48000;
        block.nChannels = 2;
        block.lpRawAudio = audio_buf.data();
        block.nSamples = static_cast<INT32>(SAMPLES_PER_FRAME);

        if (tt_) {
            TT_InsertAudioBlock(tt_, &block);
        }

        total_frames_sent++;
        position_sec_ = start_offset_sec + static_cast<int>(total_frames_sent * 20 / 1000);

        // Precise frame pacing: 20ms per frame
        next_frame_time += std::chrono::milliseconds(20);
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_for(next_frame_time - now);
        } else {
            next_frame_time = now;
        }
    }

    close(read_fd);
    kill_process();

    if (tt_) {
        TT_InsertAudioBlock(tt_, nullptr);
    }

    is_playing_ = false;

    // Trigger on_finished if it was not stopped manually
    if (!stop_requested_.load() && on_finished_) {
        on_finished_();
    }
}

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
#ifdef _WIN32
    if (current_pid_ != nullptr && current_pid_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(current_pid_, 1);
        WaitForSingleObject(current_pid_, 1000);
        CloseHandle(current_pid_);
        current_pid_ = nullptr;
    }
#else
    if (current_pid_ > 0) {
        kill(current_pid_, SIGKILL);
        waitpid(current_pid_, nullptr, 0);
        current_pid_ = -1;
    }
#endif
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
        if (std::this_thread::get_id() == worker_thread_.get_id()) {
            worker_thread_.detach();
        } else {
            worker_thread_.join();
        }
    }

    is_playing_ = false;
    is_paused_ = false;
    position_sec_ = 0;

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
        restart_stream(t, absolute_sec);
    }
}

void AudioStreamer::restart_stream(const Track& track, int offset_sec) {
    stop_requested_ = true;
    kill_process();

    if (worker_thread_.joinable()) {
        if (std::this_thread::get_id() == worker_thread_.get_id()) {
            worker_thread_.detach();
        } else {
            worker_thread_.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        current_track_ = track;
    }

    stop_requested_ = false;
    is_paused_ = false;
    is_playing_ = true;
    position_sec_ = offset_sec;

    int stream_id = ++stream_id_counter_;
    worker_thread_ = std::thread(&AudioStreamer::streaming_loop, this, track, offset_sec, stream_id);
}

void AudioStreamer::set_volume(int vol_0_100) {
    volume_ = std::clamp(vol_0_100, 0, 100);
}

int AudioStreamer::get_volume() const {
    return volume_.load();
}

void AudioStreamer::set_speed(float spd) {
    speed_ = std::clamp(spd, 0.5f, 2.0f);
    apply_filter_change();
}

float AudioStreamer::get_speed() const {
    return speed_.load();
}

void AudioStreamer::apply_filter_change() {
    Track t;
    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        t = current_track_;
    }
    if (!t.stream_url.empty() && is_playing_.load()) {
        int cur_pos = position_sec_.load();
        int resume_sec = std::max(0, cur_pos);
        restart_stream(t, resume_sec);
    }
}

void AudioStreamer::set_bass_boost(int level_db) {
    int clamped = std::clamp(level_db, 0, 20);
    if (bass_boost_.exchange(clamped) != clamped) {
        apply_filter_change();
    }
}

int AudioStreamer::get_bass_boost() const {
    return bass_boost_.load();
}

void AudioStreamer::set_nightcore(bool enable) {
    if (enable) {
        slowed_ = false;
    }
    if (nightcore_.exchange(enable) != enable) {
        apply_filter_change();
    }
}

bool AudioStreamer::get_nightcore() const {
    return nightcore_.load();
}

void AudioStreamer::set_slowed(bool enable) {
    if (enable) {
        nightcore_ = false;
    }
    if (slowed_.exchange(enable) != enable) {
        apply_filter_change();
    }
}

bool AudioStreamer::get_slowed() const {
    return slowed_.load();
}

void AudioStreamer::set_8d(bool enable) {
    if (audio_8d_.exchange(enable) != enable) {
        apply_filter_change();
    }
}

bool AudioStreamer::get_8d() const {
    return audio_8d_.load();
}

void AudioStreamer::reset_effects() {
    bool changed = (bass_boost_ > 0) || nightcore_ || slowed_ || audio_8d_ || (std::abs(speed_ - 1.0f) > 0.01f);
    bass_boost_ = 0;
    nightcore_ = false;
    slowed_ = false;
    audio_8d_ = false;
    speed_ = 1.0f;
    if (changed) {
        apply_filter_change();
    }
}

std::string AudioStreamer::get_effects_description(const std::string& lang) const {
    std::vector<std::string> active;
    if (bass_boost_.load() > 0) {
        active.push_back("Bass Boost (+" + std::to_string(bass_boost_.load()) + " dB)");
    }
    if (nightcore_.load()) {
        active.push_back("Nightcore");
    }
    if (slowed_.load()) {
        active.push_back("Slowed");
    }
    if (audio_8d_.load()) {
        active.push_back("8D Audio");
    }
    if (std::abs(speed_.load() - 1.0f) > 0.01f) {
        char buf[32];
        if (lang == "en") {
            snprintf(buf, sizeof(buf), "Speed %.2fx", speed_.load());
        } else {
            snprintf(buf, sizeof(buf), "Скорость %.2fx", speed_.load());
        }
        active.push_back(buf);
    }
    if (active.empty()) {
        return lang == "en" ? "disabled" : "отключены";
    }
    std::string res;
    for (size_t i = 0; i < active.size(); ++i) {
        if (i > 0) res += ", ";
        res += active[i];
    }
    return res;
}

std::string AudioStreamer::build_filter_chain() const {
    std::vector<std::string> filters;

    int bb = bass_boost_.load();
    if (bb > 0) {
        filters.push_back("bass=g=" + std::to_string(bb) + ":f=110:w=0.6");
    }

    if (nightcore_.load()) {
        filters.push_back("asetrate=48000*1.25,aresample=48000");
    } else if (slowed_.load()) {
        filters.push_back("asetrate=48000*0.85,aresample=48000");
    }

    if (audio_8d_.load()) {
        filters.push_back("apulsator=hz=0.125");
    }

    float spd = speed_.load();
    if (std::abs(spd - 1.0f) > 0.01f) {
        filters.push_back("atempo=" + std::to_string(spd));
    }

    if (filters.empty()) {
        return "anull";
    }

    std::string res;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i > 0) res += ",";
        res += filters[i];
    }
    return res;
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
#ifdef _WIN32
    HANDLE hPipeRead = nullptr, hPipeWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
        is_playing_ = false;
        return;
    }
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "ffmpeg.exe -y";
    if (start_offset_sec > 0) cmd += " -ss " + std::to_string(start_offset_sec);
    cmd += " -probesize 1048576 -analyzeduration 2000000";
    cmd += " -reconnect 1 -reconnect_streamed 1 -reconnect_at_eof 1 -reconnect_on_network_error 1 -reconnect_delay_max 5";
    cmd += " -user_agent \"Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 Version/18.5 Mobile/15E148 Safari/604.1\"";
    cmd += " -i \"" + track.stream_url + "\"";
    std::string filter_chain = build_filter_chain();
    cmd += " -filter:a \"" + filter_chain + "\" -f s16le -ar 48000 -ac 2 -loglevel quiet pipe:1";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hPipeWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};

    BOOL success = CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hPipeWrite);

    if (!success) {
        CloseHandle(hPipeRead);
        is_playing_ = false;
        return;
    }
    current_pid_ = pi.hProcess;
    CloseHandle(pi.hThread);
#else
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        is_playing_ = false;
        return;
    }

#ifdef __linux__
    // Expand pipe buffer from 64 KB (340ms) to 1 MB (5.4s) to absorb network jitter
    fcntl(pipefd[0], F_SETPIPE_SZ, 1048576);
    fcntl(pipefd[1], F_SETPIPE_SZ, 1048576);
#endif

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        is_playing_ = false;
        return;
    }

    if (pid == 0) {
        // Child process: ffmpeg decoder
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<std::string> arg_list;
        arg_list.push_back("ffmpeg");

        if (start_offset_sec > 0) {
            arg_list.push_back("-ss");
            arg_list.push_back(std::to_string(start_offset_sec));
        }

        arg_list.push_back("-probesize");
        arg_list.push_back("1048576");
        arg_list.push_back("-analyzeduration");
        arg_list.push_back("2000000");

        arg_list.push_back("-reconnect");
        arg_list.push_back("1");
        arg_list.push_back("-reconnect_streamed");
        arg_list.push_back("1");
        arg_list.push_back("-reconnect_at_eof");
        arg_list.push_back("1");
        arg_list.push_back("-reconnect_on_network_error");
        arg_list.push_back("1");
        arg_list.push_back("-reconnect_delay_max");
        arg_list.push_back("5");

        arg_list.push_back("-user_agent");
        arg_list.push_back("Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 Version/18.5 Mobile/15E148 Safari/604.1");

        arg_list.push_back("-i");
        arg_list.push_back(track.stream_url);

        std::string filter_chain = build_filter_chain();
        arg_list.push_back("-filter:a");
        arg_list.push_back(filter_chain);

        arg_list.push_back("-f");
        arg_list.push_back("s16le");
        arg_list.push_back("-ar");
        arg_list.push_back("48000");
        arg_list.push_back("-ac");
        arg_list.push_back("2");
        arg_list.push_back("-loglevel");
        arg_list.push_back("quiet");
        arg_list.push_back("pipe:1");

        std::vector<const char*> args;
        for (const auto& a : arg_list) {
            args.push_back(a.c_str());
        }
        args.push_back(nullptr);

        execvp("ffmpeg", const_cast<char* const*>(args.data()));
        _exit(1);
    }

    // Parent process
    close(pipefd[1]);
    current_pid_ = pid;
#endif

    const size_t SAMPLES_PER_FRAME = 960;
    const size_t CHANNELS = 2;
    const size_t BYTES_PER_FRAME = SAMPLES_PER_FRAME * CHANNELS * sizeof(int16_t);

    std::vector<int16_t> audio_buf(SAMPLES_PER_FRAME * CHANNELS);
    char* byte_ptr = reinterpret_cast<char*>(audio_buf.data());

    auto next_frame_time = std::chrono::steady_clock::now();
    uint64_t total_frames_sent = 0;
    double pos_ms = 0.0;

    while (!stop_requested_.load()) {
        if (is_paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_frame_time = std::chrono::steady_clock::now();
            continue;
        }

        size_t bytes_read = 0;
        while (bytes_read < BYTES_PER_FRAME && !stop_requested_.load()) {
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(hPipeRead, byte_ptr + bytes_read, static_cast<DWORD>(BYTES_PER_FRAME - bytes_read), &n, nullptr) || n == 0) {
                break;
            }
#else
            ssize_t n = read(pipefd[0], byte_ptr + bytes_read, BYTES_PER_FRAME - bytes_read);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) {
                break;
            }
#endif
            bytes_read += n;
        }

        if (bytes_read < BYTES_PER_FRAME) {
            break;
        }

        int vol = volume_.load();
        if (vol <= 0) {
            std::memset(audio_buf.data(), 0, audio_buf.size() * sizeof(int16_t));
        } else if (vol < 100) {
            float norm = vol / 100.0f;
            float factor = norm * norm;
            for (size_t i = 0; i < audio_buf.size(); ++i) {
                audio_buf[i] = static_cast<int16_t>(audio_buf[i] * factor);
            }
        }

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
        double speed_mult = 1.0;
        if (nightcore_.load()) {
            speed_mult = 1.25;
        } else if (slowed_.load()) {
            speed_mult = 0.85;
        } else {
            speed_mult = speed_.load();
        }

        pos_ms += 20.0 * speed_mult;
        position_sec_ = start_offset_sec + static_cast<int>(pos_ms / 1000.0);

        next_frame_time += std::chrono::milliseconds(20);
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_for(next_frame_time - now);
        } else {
            next_frame_time = now;
        }
    }

#ifdef _WIN32
    if (hPipeRead) CloseHandle(hPipeRead);
#else
    close(pipefd[0]);
#endif
    kill_process();

    if (tt_) {
        TT_InsertAudioBlock(tt_, nullptr);
    }

    is_playing_ = false;

    if (!stop_requested_.load() && on_finished_) {
        on_finished_();
    }
}
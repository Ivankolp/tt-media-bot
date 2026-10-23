#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class PlaybackMode {
    Normal,
    RepeatOne,
    RepeatAll,
    Random
};

struct Track {
    std::string id;
    std::string title;
    std::string uploader;
    std::string webpage_url;
    std::string stream_url;
    int duration_sec = 0;
};

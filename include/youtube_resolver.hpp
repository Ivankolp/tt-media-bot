#pragma once
#include <string>
#include <vector>
#include "types.hpp"

class YouTubeResolver {
public:
    explicit YouTubeResolver(std::string bridge_url);
    ~YouTubeResolver() = default;

    std::vector<Track> search(const std::string& query, int limit = 30);
    bool resolve_stream(Track& track);
    bool resolve_direct_url(const std::string& url, Track& out_track);
    std::vector<Track> resolve_playlist(const std::string& url);
    std::vector<Track> get_recommendations(const std::string& video_id, int limit = 25);

private:
    std::string bridge_url_;
    std::string http_post(const std::string& endpoint, const std::string& json_payload);
    std::string exec_command(const std::string& cmd);
};
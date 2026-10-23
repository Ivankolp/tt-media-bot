#pragma once
#include <string>
#include <vector>
#include "types.hpp"

class YouTubeResolver {
public:
    explicit YouTubeResolver(std::string bridge_url);
    ~YouTubeResolver();

    std::vector<Track> search(const std::string& query, int limit = 5);
    bool resolve_stream(Track& track);
    bool resolve_direct_url(const std::string& url, Track& out_track);

private:
    std::string bridge_url_;
    
    std::string http_post(const std::string& endpoint, const std::string& json_payload);
    std::string exec_command(const std::string& cmd);
};

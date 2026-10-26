#include "youtube_resolver.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <array>
#include <memory>
#include <curl/curl.h>

static size_t on_curl_write_data(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

YouTubeResolver::YouTubeResolver(std::string bridge_url)
    : bridge_url_(std::move(bridge_url)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::string YouTubeResolver::http_post(const std::string& endpoint, const std::string& json_payload) {
    if (bridge_url_.empty()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string full_url = bridge_url_ + endpoint;
    std::string response_string;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_curl_write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && http_code == 200) {
        return response_string;
    }
    return "";
}

std::string YouTubeResolver::exec_command(const std::string& cmd) {
    std::array<char, 256> buffer{};
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::vector<Track> YouTubeResolver::search(const std::string& query, int limit) {
    std::vector<Track> tracks;

    // 1. Try local node bridge first
    nlohmann::json req;
    req["query"] = query;
    req["limit"] = limit;
    std::string res = http_post("/search", req.dump());
    if (!res.empty()) {
        try {
            auto j = nlohmann::json::parse(res);
            if (j.contains("entries") && j["entries"].is_array()) {
                for (const auto& item : j["entries"]) {
                    if (static_cast<int>(tracks.size()) >= limit) break;
                    Track t;
                    t.id = item.value("id", item.value("videoId", ""));
                    t.title = item.value("title", "Unknown Title");
                    t.uploader = item.value("uploader", "Unknown Artist");
                    t.webpage_url = item.value("webpage_url", "https://www.youtube.com/watch?v=" + t.id);
                    t.duration_sec = item.value("duration", 0);
                    if (item.contains("url") && item["url"].is_string() && !item["url"].get<std::string>().empty()) {
                        t.stream_url = item["url"].get<std::string>();
                    } else if (item.contains("stream_url") && item["stream_url"].is_string() && !item["stream_url"].get<std::string>().empty()) {
                        t.stream_url = item["stream_url"].get<std::string>();
                    }
                    if (!t.id.empty()) {
                        tracks.push_back(t);
                    }
                }
                if (!tracks.empty()) return tracks;
            }
        } catch (...) {}
    }

    // 2. Fallback to yt-dlp
    int fallback_limit = std::min(limit, 5);
    std::string cmd = "yt-dlp --socket-timeout 3 --default-search \"ytsearch" + std::to_string(fallback_limit) + ":\" --print \"%(id)s\\t%(title)s\\t%(uploader)s\\t%(duration)s\" --no-warnings --no-playlist \"" + query + "\" 2>/dev/null";
    std::string output = exec_command(cmd);
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::istringstream line_stream(line);
        std::string id, title, uploader, dur_str;
        if (std::getline(line_stream, id, '\t') &&
            std::getline(line_stream, title, '\t') &&
            std::getline(line_stream, uploader, '\t') &&
            std::getline(line_stream, dur_str, '\t')) {
            Track t;
            t.id = id;
            t.title = title;
            t.uploader = uploader;
            t.webpage_url = "https://www.youtube.com/watch?v=" + id;
            try { t.duration_sec = std::stoi(dur_str); } catch (...) {}
            tracks.push_back(t);
        }
    }
    return tracks;
}

std::vector<Track> YouTubeResolver::resolve_playlist(const std::string& url) {
    std::vector<Track> tracks;
    std::string cmd = "yt-dlp --socket-timeout 4 --flat-playlist --print \"%(id)s\\t%(title)s\\t%(uploader)s\\t%(duration)s\" --no-warnings \"" + url + "\" 2>/dev/null";
    std::string output = exec_command(cmd);
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::istringstream line_stream(line);
        std::string id, title, uploader, dur_str;
        if (std::getline(line_stream, id, '\t') &&
            std::getline(line_stream, title, '\t') &&
            std::getline(line_stream, uploader, '\t') &&
            std::getline(line_stream, dur_str, '\t')) {
            Track t;
            t.id = id;
            t.title = title;
            t.uploader = uploader;
            t.webpage_url = "https://www.youtube.com/watch?v=" + id;
            try { t.duration_sec = std::stoi(dur_str); } catch (...) {}
            tracks.push_back(t);
        }
    }
    return tracks;
}

std::vector<Track> YouTubeResolver::get_recommendations(const std::string& video_id, int limit) {
    std::vector<Track> tracks;
    if (video_id.empty() || video_id.find("http") == 0) return tracks;

    nlohmann::json req;
    req["video_id"] = video_id;
    req["limit"] = limit;
    std::string res = http_post("/recommendations", req.dump());
    if (!res.empty()) {
        try {
            auto j = nlohmann::json::parse(res);
            if (j.contains("entries") && j["entries"].is_array()) {
                for (const auto& item : j["entries"]) {
                    if (static_cast<int>(tracks.size()) >= limit) break;
                    Track t;
                    t.id = item.value("id", item.value("videoId", ""));
                    t.title = item.value("title", "Unknown Title");
                    t.uploader = item.value("uploader", "Unknown Artist");
                    t.webpage_url = item.value("webpage_url", "https://www.youtube.com/watch?v=" + t.id);
                    t.duration_sec = item.value("duration", 0);
                    if (!t.id.empty()) {
                        tracks.push_back(t);
                    }
                }
            }
        } catch (...) {}
    }
    return tracks;
}

bool YouTubeResolver::resolve_stream(Track& track) {
    if (!track.stream_url.empty()) return true;

    // 1. Try local node bridge
    nlohmann::json req;
    req["video_id"] = track.id;
    std::string res = http_post("/resolve", req.dump());
    if (!res.empty()) {
        try {
            auto j = nlohmann::json::parse(res);
            if (j.contains("url") && j["url"].is_string()) {
                track.stream_url = j["url"].get<std::string>();
                if (j.contains("title")) track.title = j["title"].get<std::string>();
                if (j.contains("uploader")) track.uploader = j["uploader"].get<std::string>();
                if (j.contains("duration")) track.duration_sec = j["duration"].get<int>();
                return true;
            }
        } catch (...) {}
    }

    // 2. Fallback to yt-dlp
    std::string target = track.webpage_url.empty() ? ("https://www.youtube.com/watch?v=" + track.id) : track.webpage_url;
    std::string cmd = "yt-dlp --socket-timeout 3 --no-warnings -g -f \"ba/b\" \"" + target + "\" 2>/dev/null";
    std::string direct_url = exec_command(cmd);
    if (!direct_url.empty() && direct_url.find("http") == 0) {
        track.stream_url = direct_url;
        return true;
    }
    return false;
}

bool YouTubeResolver::resolve_direct_url(const std::string& url, Track& out_track) {
    out_track.webpage_url = url;
    out_track.id = url;
    out_track.title = url;
    out_track.duration_sec = 0;
    out_track.uploader = "Direct Stream / Radio";

    bool is_youtube = (url.find("youtube.com") != std::string::npos ||
                       url.find("youtu.be") != std::string::npos);

    if (!is_youtube) {
        out_track.stream_url = url;
        size_t last_slash = url.find_last_of('/');
        if (last_slash != std::string::npos && last_slash + 1 < url.size()) {
            std::string path_part = url.substr(last_slash + 1);
            size_t query_pos = path_part.find('?');
            if (query_pos != std::string::npos) path_part = path_part.substr(0, query_pos);
            if (!path_part.empty()) out_track.title = path_part;
        }
        return true;
    }

    std::string video_id;
    size_t vpos = url.find("v=");
    if (vpos != std::string::npos && vpos + 2 < url.size()) {
        video_id = url.substr(vpos + 2, 11);
    } else {
        size_t bepos = url.find("youtu.be/");
        if (bepos != std::string::npos && bepos + 9 < url.size()) {
            video_id = url.substr(bepos + 9, 11);
        }
    }

    if (!video_id.empty()) {
        out_track.id = video_id;
        if (resolve_stream(out_track)) {
            return true;
        }
    }

    std::string cmd = "yt-dlp --socket-timeout 3 --print \"%(id)s\\t%(title)s\\t%(uploader)s\\t%(duration)s\" --no-warnings \"" + url + "\" 2>/dev/null";
    std::string output = exec_command(cmd);
    std::istringstream stream(output);
    std::string line;
    if (std::getline(stream, line) && !line.empty()) {
        std::istringstream line_stream(line);
        std::string id, title, uploader, dur_str;
        if (std::getline(line_stream, id, '\t') &&
            std::getline(line_stream, title, '\t') &&
            std::getline(line_stream, uploader, '\t') &&
            std::getline(line_stream, dur_str, '\t')) {
            out_track.id = id;
            out_track.title = title;
            out_track.uploader = uploader;
            try { out_track.duration_sec = std::stoi(dur_str); } catch (...) {}
        }
    }

    return resolve_stream(out_track);
}
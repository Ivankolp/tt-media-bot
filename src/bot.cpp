#include "bot.hpp"
#include "logger.hpp"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <sys/stat.h>

extern char** g_app_argv;

Bot::Bot(BotConfig config)
    : config_(std::move(config)) {}

Bot::~Bot() {
    stop();
    if (tt_) {
        TT_CloseTeamTalk(tt_);
        tt_ = nullptr;
    }
}

bool Bot::init() {
    tt_ = TT_InitTeamTalkPoll();
    if (!tt_) {
        LOG_ERR("[Bot] Failed to initialize TeamTalk SDK!");
        return false;
    }

    db_ = std::make_unique<Database>(config_.db_path);
    if (!db_->init()) {
        LOG_ERR("[Bot] Failed to initialize SQLite database: %s", config_.db_path.c_str());
    } else {
        // Seed initial admins and settings from config if not present
        for (const auto& a : config_.admin_users) {
            db_->add_admin_user(a);
        }
        for (const auto& b : config_.banned_users) {
            db_->ban_user(b);
        }
        for (const auto& c : config_.blocked_commands) {
            db_->block_command(c);
        }
    }

    resolver_ = std::make_unique<YouTubeResolver>(config_.bridge_url);
    streamer_ = std::make_unique<AudioStreamer>(tt_);
    std::string data_dir = ".";
    try {
        std::filesystem::path p(config_.db_path);
        if (p.has_parent_path()) {
            data_dir = p.parent_path().string();
        }
    } catch (...) {}
    playlist_ = std::make_unique<PlaylistManager>(data_dir);

    // Load persistent volume from SQLite DB
    int saved_vol = db_ ? db_->get_volume(config_.default_volume) : config_.default_volume;
    streamer_->set_volume(saved_vol);

    // Load persistent mode
    PlaybackMode saved_mode = db_ ? db_->get_mode(PlaybackMode::Normal) : PlaybackMode::Normal;
    playlist_->set_mode(saved_mode);

    // Load persistent speed
    float saved_spd = db_ ? db_->get_speed(1.0f) : 1.0f;
    streamer_->set_speed(saved_spd);

    // Load persistent effects
    if (db_) {
        int bb = db_->get_int("fx_bass", 0);
        bool nc = (db_->get_int("fx_nc", 0) != 0);
        bool sl = (db_->get_int("fx_slow", 0) != 0);
        bool e8d = (db_->get_int("fx_8d", 0) != 0);
        if (bb > 0) streamer_->set_bass_boost(bb);
        if (nc) streamer_->set_nightcore(true);
        if (sl) streamer_->set_slowed(true);
        if (e8d) streamer_->set_8d(true);
    }

    // Load persistent status
    if (db_) {
        std::string saved_st = db_->get_status(config_.status_message);
        if (!saved_st.empty()) {
            config_.status_message = saved_st;
        }

        std::string saved_nick = db_->get_setting("nickname");
        if (!saved_nick.empty()) {
            config_.nickname = saved_nick;
        }

        std::string saved_chan = db_->get_setting("channel_path");
        if (!saved_chan.empty()) {
            config_.channel_path = saved_chan;
        }

        std::string saved_gender = db_->get_setting("gender");
        if (!saved_gender.empty()) {
            config_.gender = saved_gender;
        }

        std::string saved_lang = db_->get_setting("language");
        if (!saved_lang.empty()) {
            config_.language = saved_lang;
        }

        std::string saved_cm = db_->get_setting("channel_messages");
        if (!saved_cm.empty()) {
            config_.channel_messages = (saved_cm == "1");
        }

        std::string saved_vtx = db_->get_setting("voice_tx");
        if (!saved_vtx.empty()) {
            voice_transmission_enabled_ = (saved_vtx == "1");
        }

        std::string saved_locked = db_->get_setting("locked");
        if (!saved_locked.empty()) {
            locked_ = (saved_locked == "1");
        }

        search_limit_ = db_->get_int("search_limit", 5);

        auto saved_q = db_->load_queue();
        for (const auto& t : saved_q) {
            playlist_->add_to_queue(t);
        }
    }

    streamer_->set_on_finished_callback([this]() {
        std::thread([this]() {
            on_track_finished();
        }).detach();
    });

    LOG_INFO("[Bot] Initialized native bot engine successfully. Volume=%d%%, Mode=%s",
             saved_vol, playlist_->get_mode_name().c_str());
    return true;
}

void Bot::run() {
    running_ = true;
    connect_and_login();

    INT32 wait_ms = 50;
    TTMessage tt_msg{};

    while (running_.load()) {
        if (TT_GetMessage(tt_, &tt_msg, &wait_ms)) {
            process_event(tt_msg);
        }
    }

    if (tt_) {
        TT_Disconnect(tt_);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        TT_CloseTeamTalk(tt_);
        tt_ = nullptr;
    }
}

void Bot::stop() {
    running_ = false;
    if (streamer_) {
        streamer_->stop();
    }
    if (tt_) {
        TT_DoQuit(tt_);
        TT_Disconnect(tt_);
    }
}

void Bot::restart_process() {
    LOG_INFO("[Bot] Fast restart requested via execv...");
    stop();
    if (tt_) {
        TT_CloseTeamTalk(tt_);
        tt_ = nullptr;
    }
    std::fflush(stdout);
    std::fflush(stderr);

    if (g_app_argv && g_app_argv[0]) {
        execv("/proc/self/exe", g_app_argv);
        execv(g_app_argv[0], g_app_argv);
    }
    std::exit(42);
}

void Bot::connect_and_login() {
    LOG_INFO("[Bot] Connecting to TeamTalk server %s:%d...", config_.host.c_str(), config_.tcp_port);
    if (tt_) {
        TT_Disconnect(tt_);
    }
    TTBOOL ok = TT_Connect(tt_, config_.host.c_str(), config_.tcp_port, config_.udp_port, 0, 0, config_.encrypted ? 1 : 0);
    if (!ok) {
        LOG_ERR("[Bot] TT_Connect failed! Retrying in %d seconds...", config_.reconnection_timeout);
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(config_.reconnection_timeout));
            if (running_.load()) {
                connect_and_login();
            }
        }).detach();
    }
}

void Bot::process_event(const TTMessage& msg) {
    switch (msg.nClientEvent) {
        case CLIENTEVENT_CON_SUCCESS: {
            LOG_INFO("[Bot] Connected to server successfully! Logging in as '%s'...", config_.username.c_str());
            TT_DoLogin(tt_, config_.nickname.c_str(), config_.username.c_str(), config_.password.c_str());
            break;
        }
        case CLIENTEVENT_CON_FAILED: {
            LOG_ERR("[Bot] Connection failed! Reconnecting in %d seconds...", config_.reconnection_timeout);
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::seconds(config_.reconnection_timeout));
                if (running_.load()) {
                    connect_and_login();
                }
            }).detach();
            break;
        }
        case CLIENTEVENT_CON_LOST: {
            LOG_ERR("[Bot] Connection lost! Reconnecting in 3 seconds...");
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (running_.load()) {
                    connect_and_login();
                }
            }).detach();
            break;
        }
        case CLIENTEVENT_CMD_MYSELF_LOGGEDIN: {
            LOG_INFO("[Bot] Logged in successfully! Joining channel...");
            TT_DoChangeNickname(tt_, config_.nickname.c_str());

            std::string st = db_ ? db_->get_status(config_.status_message) : config_.status_message;
            if (st.empty()) st = (config_.language == "en") ? "Send h for help" : "Отправьте h для справки";
            TT_DoChangeStatus(tt_, get_gender_mode(), st.c_str());
            
            // Join channel
            if (!config_.channel_path.empty() && config_.channel_path != "/") {
                try {
                    int cid = std::stoi(config_.channel_path);
                    TT_DoJoinChannelByID(tt_, cid, config_.channel_password.c_str());
                } catch (...) {
                    Channel ch{};
                    std::strncpy(ch.szName, config_.channel_path.c_str(), sizeof(ch.szName) - 1);
                    std::strncpy(ch.szPassword, config_.channel_password.c_str(), sizeof(ch.szPassword) - 1);
                    TT_DoJoinChannel(tt_, &ch);
                }
            } else {
                TT_DoJoinChannelByID(tt_, 1, config_.channel_password.c_str());
            }

            if (!config_.start_commands.empty()) {
                std::thread([this]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    for (const auto& cmd : config_.start_commands) {
                        if (!cmd.empty()) {
                            LOG_INFO("[Bot] Executing start command: %s", cmd.c_str());
                            TextMessage fake_msg{};
                            fake_msg.nFromUserID = 0;
                            std::strncpy(fake_msg.szFromUsername, "admin", sizeof(fake_msg.szFromUsername) - 1);
                            std::strncpy(fake_msg.szMessage, cmd.c_str(), sizeof(fake_msg.szMessage) - 1);
                            fake_msg.nMsgType = MSGTYPE_USER;
                            handle_text_message(fake_msg);
                        }
                    }
                }).detach();
            }
            break;
        }
        case CLIENTEVENT_CMD_USER_TEXTMSG: {
            handle_text_message(msg.textmessage);
            break;
        }
        case CLIENTEVENT_FILETRANSFER: {
            const FileTransfer& ft = msg.filetransfer;
            LOG_INFO("[FileTransfer] id=%d status=%d bytes=%lld/%lld file='%s'",
                     ft.nTransferID, (int)ft.nStatus, (long long)ft.nTransferred, (long long)ft.nFileSize, ft.szRemoteFileName);
            break;
        }
        case CLIENTEVENT_CMD_FILE_NEW: {
            LOG_INFO("[FileTransfer] File uploaded to channel %d: id=%d name='%s' size=%lld",
                     msg.remotefile.nChannelID, msg.remotefile.nFileID, msg.remotefile.szFileName, (long long)msg.remotefile.nFileSize);
            break;
        }
        case CLIENTEVENT_CMD_ERROR: {
            LOG_ERR("[Bot] Command error (cmd_id=%d, err_code=%d): %s",
                    msg.nSource, msg.clienterrormsg.nErrorNo, msg.clienterrormsg.szErrorMsg);
            break;
        }
        default:
            break;
    }
}

int Bot::get_gender_mode() const {
    if (config_.gender == "f") return 0x00000100; // STATUSMODE_FEMALE (256)
    if (config_.gender == "n") return 0x00001000; // STATUSMODE_NEUTRAL (4096)
    return 0x00000000; // STATUSMODE_MALE (0)
}

void Bot::send_message(int to_user_id, int channel_id, const std::string& text) {
    if (!tt_ || text.empty()) return;
    if (channel_id > 0 && !config_.channel_messages) return;

    // Split text into chunks that fit within TT_STRLEN (512 bytes)
    // Safe chunk size: 360 bytes, splitting by lines and keeping multi-byte UTF-8 intact
    std::vector<std::string> chunks;
    std::istringstream stream(text);
    std::string line;
    std::string current_chunk;

    while (std::getline(stream, line)) {
        // If single line is exceptionally long, split it safely on UTF-8 boundaries
        while (line.size() > 360) {
            size_t cut = 360;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
                cut--;
            }
            if (!current_chunk.empty()) {
                chunks.push_back(current_chunk);
                current_chunk.clear();
            }
            chunks.push_back(line.substr(0, cut));
            line = line.substr(cut);
        }

        if (current_chunk.empty()) {
            current_chunk = line;
        } else if (current_chunk.size() + 1 + line.size() <= 360) {
            current_chunk += "\n" + line;
        } else {
            chunks.push_back(current_chunk);
            current_chunk = line;
        }
    }
    if (!current_chunk.empty()) {
        chunks.push_back(current_chunk);
    }

    for (const auto& chunk : chunks) {
        TextMessage tm{};
        if (channel_id > 0) {
            tm.nMsgType = MSGTYPE_CHANNEL;
            tm.nChannelID = channel_id;
            tm.nToUserID = 0;
        } else {
            tm.nMsgType = MSGTYPE_USER;
            tm.nChannelID = 0;
            tm.nToUserID = to_user_id;
        }
        std::strncpy(tm.szMessage, chunk.c_str(), sizeof(tm.szMessage) - 1);
        INT32 cmd_id = TT_DoTextMessage(tt_, &tm);
        LOG_INFO("[Chat] Sent message chunk (cmd_id=%d, len=%zu): to_chan=%d, to_user=%d",
                 cmd_id, chunk.size(), channel_id, to_user_id);
    }
}

void Bot::reply(const TextMessage& original, const std::string& text) {
    if (original.nFromUserID > 0) {
        send_message(original.nFromUserID, 0, text);
    } else {
        send_message(0, TT_GetMyChannelID(tt_), text);
    }
}

bool Bot::is_admin(const std::string& username) const {
    if (db_ && db_->is_admin_user(username)) return true;
    for (const auto& a : config_.admin_users) {
        if (a == username) return true;
    }
    return false;
}

bool Bot::is_banned(const std::string& username) const {
    if (db_ && db_->is_user_banned(username)) return true;
    for (const auto& b : config_.banned_users) {
        if (b == username) return true;
    }
    return false;
}

bool Bot::is_command_blocked(const std::string& cmd) const {
    if (db_ && db_->is_command_blocked(cmd)) return true;
    for (const auto& c : config_.blocked_commands) {
        if (c == cmd) return true;
    }
    return false;
}

void Bot::handle_text_message(const TextMessage& msg) {
    if (msg.nFromUserID == TT_GetMyUserID(tt_)) return;

    // Strict rule: only handle commands from private messages, completely ignore channel messages
    if (msg.nMsgType != MSGTYPE_USER) {
        return;
    }

    LOG_INFO("[Chat] Received PM from '%s' (user_id=%d): '%s'",
             msg.szFromUsername, msg.nFromUserID, msg.szMessage);

    std::string raw = msg.szMessage;
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    if (raw.empty()) return;

    if (raw.front() == '!' || raw.front() == '/') {
        raw.erase(raw.begin());
    }

    std::istringstream iss(raw);
    std::string cmd;
    iss >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    std::string arg;
    std::getline(iss, arg);
    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());

    std::string user_name = msg.szFromUsername;

    // Check ban
    if (is_banned(user_name)) {
        reply(msg, tr("Ошибка: Вы заблокированы", "Error: You are banned"));
        return;
    }

    // Check locked bot
    if (locked_ && !is_admin(user_name)) {
        reply(msg, tr("Ошибка: Бот заблокирован администратором", "Error: Bot is locked by administrator"));
        return;
    }

    // Check blocked command
    if (is_command_blocked(cmd) && !is_admin(user_name)) {
        reply(msg, tr("Ошибка: Команда заблокирована", "Error: Command is blocked"));
        return;
    }

    // User commands
    if (cmd == "p" || cmd == "play") {
        cmd_play(msg, arg);
    } else if (cmd == "u" || cmd == "playurl") {
        cmd_play_url(msg, arg);
    } else if (cmd == "s" || cmd == "stop") {
        cmd_stop(msg);
    } else if (cmd == "b" || cmd == "back") {
        cmd_back(msg);
    } else if (cmd == "n" || cmd == "next") {
        cmd_next(msg, arg);
    } else if (cmd == "c" || cmd == "current") {
        cmd_current(msg, arg);
    } else if (cmd == "sb") {
        int sec = config_.seek_step;
        try { if (!arg.empty()) sec = std::stoi(arg); } catch (...) {}
        cmd_seek(msg, -sec);
    } else if (cmd == "sf") {
        int sec = config_.seek_step;
        try { if (!arg.empty()) sec = std::stoi(arg); } catch (...) {}
        cmd_seek(msg, sec);
    } else if (cmd == "v" || cmd == "vol" || cmd == "volume") {
        cmd_volume(msg, arg);
    } else if (cmd == "sp" || cmd == "speed") {
        cmd_speed(msg, arg);
    } else if (cmd == "bb" || cmd == "bass" || cmd == "bassboost") {
        cmd_bass(msg, arg);
    } else if (cmd == "nc" || cmd == "nightcore") {
        cmd_nightcore(msg, arg);
    } else if (cmd == "slow" || cmd == "slowed") {
        cmd_slowed(msg, arg);
    } else if (cmd == "8d" || cmd == "eightd") {
        cmd_8d(msg, arg);
    } else if (cmd == "fx" || cmd == "effects" || cmd == "ef") {
        cmd_effects(msg, arg);
    } else if (cmd == "m" || cmd == "mode") {
        cmd_mode(msg, arg);
    } else if (cmd == "f" || cmd == "fav") {
        cmd_favorites(msg, arg);
    } else if (cmd == "r" || cmd == "recents") {
        cmd_recents(msg);
    } else if (cmd == "gl" || cmd == "link") {
        cmd_get_link(msg);
    } else if (cmd == "qa") {
        cmd_queue_add(msg, arg);
    } else if (cmd == "ql") {
        cmd_queue_list(msg);
    } else if (cmd == "qr") {
        cmd_queue_remove(msg, arg);
    } else if (cmd == "qc") {
        cmd_queue_clear(msg);
    } else if (cmd == "qs") {
        cmd_queue_skip(msg);
    } else if (cmd == "sr") {
        cmd_search(msg, arg);
    } else if (cmd == "sl") {
        cmd_select_search(msg, arg);
    } else if (cmd == "slc") {
        cmd_select_search_count(msg, arg);
    } else if (cmd == "jc" || cmd == "come" || cmd == "go") {
        cmd_join_channel(msg, arg);
    } else if (cmd == "seek" || cmd == "st") {
        cmd_seek_timecode(msg, arg);
    } else if (cmd == "dl" || cmd == "download") {
        cmd_download(msg, arg);
    } else if (cmd == "sv") {
        cmd_service(msg, arg);
    } else if (cmd == "h" || cmd == "help") {
        cmd_help(msg, arg);
    } else if (cmd == "a" || cmd == "about") {
        cmd_about(msg);
    }
    // Admin commands
    else if (cmd == "cn") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_change_nick(msg, arg);
    } else if (cmd == "cs") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_change_status(msg, arg);
    } else if (cmd == "cg") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_change_gender(msg, arg);
    } else if (cmd == "cl") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_change_language(msg, arg);
    } else if (cmd == "va") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_voice_toggle(msg, arg);
    } else if (cmd == "cm") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_channel_msg_toggle(msg, arg);
    } else if (cmd == "gcid") {
        cmd_get_channel_id(msg);
    } else if (cmd == "ajc") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_join_channel(msg, arg);
    } else if (cmd == "bc") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_block_command(msg, arg);
    } else if (cmd == "l") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_lock_bot(msg, arg);
    } else if (cmd == "ua") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_admin_users(msg, arg);
    } else if (cmd == "ub") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_banned_users(msg, arg);
    } else if (cmd == "sc") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_save_config(msg);
    } else if (cmd == "cc") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_clear_cache(msg);
    } else if (cmd == "cookies" || cmd == "cookie") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        cmd_cookies(msg, arg);
    } else if (cmd == "q" || cmd == "quit") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        reply(msg, tr("Завершение работы бота...", "Shutting down bot..."));
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            stop();
            if (tt_) {
                TT_CloseTeamTalk(tt_);
                tt_ = nullptr;
            }
            std::exit(0);
        }).detach();
    } else if (cmd == "rs" || cmd == "restart") {
        if (!is_admin(user_name)) { reply(msg, tr("Ошибка: Недостаточно прав админа", "Error: Insufficient admin permissions")); return; }
        reply(msg, tr("Перезапуск бота...", "Restarting bot..."));
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            restart_process();
        }).detach();
    }
}

bool Bot::get_next_track(Track& next) {
    if (playlist_->pop_next(next)) {
        if (db_) db_->save_queue(playlist_->get_queue());
        return true;
    }

    // If current list is exhausted, fetch recommendations based on current or last track (infinite swipe)
    Track cur = playlist_->get_current_track();
    std::string vid = cur.id;
    if (vid.empty() && !playlist_->get_track_list().empty()) {
        vid = playlist_->get_track_list().back().id;
    }

    if (!vid.empty() && vid.find("http") != 0) {
        LOG_INFO("[Bot] Reached end of current playlist. Fetching recommendations for video %s...", vid.c_str());
        auto recs = resolver_->get_recommendations(vid, 25);
        size_t added = playlist_->append_unique(recs);
        LOG_INFO("[Bot] Added %zu new recommendation tracks to playlist", added);
        if (added > 0) {
            if (playlist_->pop_next(next)) {
                return true;
            }
        }
    }

    return false;
}

void Bot::play_track(const Track& track, const TextMessage* reply_to) {
    Track t = track;
    if (t.stream_url.empty()) {
        if (!resolver_->resolve_stream(t)) {
            // Track unplayable / blocked: auto-skip to next without hanging
            if (reply_to) {
                reply(*reply_to, tr("Трек недоступен (" + t.title + "), переключаю...",
                                   "Track unavailable (" + t.title + "), skipping..."));
            }
            Track next;
            if (get_next_track(next)) {
                play_track(next, reply_to);
            } else {
                if (reply_to) reply(*reply_to, tr("Выдача закончилась", "No more tracks in playlist"));
            }
            return;
        }
    }

    playlist_->set_current_track(t);
    playlist_->add_recent(t);
    if (db_) db_->add_recent(t);
    streamer_->play(t);

    // Update TeamTalk status
    std::string st = (config_.language == "en" ? "Playing: " : "Воспроизводится: ") + t.title;
    TT_DoChangeStatus(tt_, get_gender_mode(), st.c_str());

    char buf[1024];
    int cur = playlist_->get_track_index() + 1;
    int tot = static_cast<int>(playlist_->get_track_list().size());
    const char* prefix = (config_.language == "en") ? "▶ Now playing" : "▶ Сейчас играет";
    if (tot > 1 && cur > 0) {
        if (t.duration_sec > 0) {
            snprintf(buf, sizeof(buf), "%s (%d/%d): %s [%d:%02d]",
                     prefix, cur, tot, t.title.c_str(), t.duration_sec / 60, t.duration_sec % 60);
        } else {
            snprintf(buf, sizeof(buf), "%s (%d/%d): %s",
                     prefix, cur, tot, t.title.c_str());
        }
    } else {
        if (t.duration_sec > 0) {
            snprintf(buf, sizeof(buf), "%s: %s [%d:%02d]",
                     prefix, t.title.c_str(), t.duration_sec / 60, t.duration_sec % 60);
        } else {
            snprintf(buf, sizeof(buf), "%s: %s", prefix, t.title.c_str());
        }
    }

    // STRICT: Only reply to PM of the user who called it. NEVER send to channel!
    if (reply_to) {
        reply(*reply_to, buf);
    }
}

void Bot::on_track_finished() {
    Track next;
    if (get_next_track(next)) {
        play_track(next);
    } else {
        TT_DoChangeStatus(tt_, get_gender_mode(), config_.status_message.c_str());
    }
}

void Bot::cmd_play(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        if (streamer_->is_playing()) {
            if (streamer_->is_paused()) {
                streamer_->resume();
                std::string st = (config_.language == "en" ? "Playing: " : "Воспроизводится: ") + streamer_->get_current_track().title;
                TT_DoChangeStatus(tt_, get_gender_mode(), st.c_str());
                reply(msg, tr("Воспроизведение возобновлено", "Playback resumed"));
            } else {
                streamer_->pause();
                std::string st = (config_.language == "en" ? "Paused: " : "Пауза: ") + streamer_->get_current_track().title;
                TT_DoChangeStatus(tt_, get_gender_mode(), st.c_str());
                reply(msg, tr("Воспроизведение приостановлено", "Playback paused"));
            }
            return;
        }

        Track next;
        if (get_next_track(next)) {
            play_track(next, &msg);
        } else {
            reply(msg, tr("Очередь пуста. Укажите трек: p <название или ссылка>",
                         "Queue is empty. Specify track: p <query or URL>"));
        }
        return;
    }

    reply(msg, tr("Поиск: ", "Searching: ") + arg + "...");

    // 1. YouTube playlist
    if ((arg.find("http://") == 0 || arg.find("https://") == 0) &&
        (arg.find("list=") != std::string::npos)) {
        auto pl_tracks = resolver_->resolve_playlist(arg);
        if (!pl_tracks.empty()) {
            playlist_->set_track_list(pl_tracks, 0);
            reply(msg, tr("Загружен плейлист: " + std::to_string(pl_tracks.size()) + " треков",
                         "Loaded playlist: " + std::to_string(pl_tracks.size()) + " tracks"));
            play_track(pl_tracks[0], &msg);
            return;
        }
    }

    // 2. Direct URL / single stream
    if (arg.find("http://") == 0 || arg.find("https://") == 0) {
        Track target;
        if (!resolver_->resolve_direct_url(arg, target)) {
            reply(msg, tr("Ошибка: Не удалось обработать ссылку", "Error: Failed to process URL"));
            return;
        }
        playlist_->set_track_list({target}, 0);
        play_track(target, &msg);
        return;
    }

    // 3. Search query: search 30 tracks for extended feed
    auto results = resolver_->search(arg, 30);
    if (results.empty()) {
        reply(msg, tr("Ничего не найдено по запросу: ", "Nothing found for: ") + arg);
        return;
    }

    playlist_->set_track_list(results, 0);
    play_track(results[0], &msg);
}

void Bot::cmd_play_url(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Использование: u <прямая ссылка>", "Usage: u <direct stream URL>"));
        return;
    }
    reply(msg, tr("Загрузка потока: ", "Loading stream: ") + arg + "...");
    Track t;
    if (resolver_->resolve_direct_url(arg, t)) {
        playlist_->set_track_list({t}, 0);
        play_track(t, &msg);
    } else {
        reply(msg, tr("Ошибка: Не удалось воспроизвести ссылку", "Error: Failed to play stream URL"));
    }
}

void Bot::cmd_stop(const TextMessage& msg) {
    streamer_->stop();
    playlist_->clear_all();
    if (db_) db_->clear_queue();
    TT_DoChangeStatus(tt_, get_gender_mode(), config_.status_message.c_str());
    reply(msg, tr("Воспроизведение остановлено, очередь очищена", "Playback stopped, queue cleared"));
}

void Bot::cmd_back(const TextMessage& msg) {
    Track prev;
    if (playlist_->pop_prev(prev)) {
        play_track(prev, &msg);
    } else {
        reply(msg, tr("Предыдущих треков нет", "No previous tracks"));
    }
}

void Bot::cmd_next(const TextMessage& msg, const std::string& arg) {
    if (!arg.empty()) {
        if (arg == "?") {
            if (streamer_->is_playing() && playlist_->has_tracks()) {
                int cur = playlist_->get_track_index() + 1;
                int tot = static_cast<int>(playlist_->get_track_list().size());
                Track t = streamer_->get_current_track();
                reply(msg, std::to_string(cur) + "/" + std::to_string(tot) + " - " + t.title);
                return;
            } else if (streamer_->is_playing()) {
                Track t = streamer_->get_current_track();
                reply(msg, tr("Играет: ", "Playing: ") + t.title);
                return;
            } else {
                reply(msg, tr("Сейчас ничего не играет", "Nothing is currently playing"));
                return;
            }
        }
        try {
            int idx = std::stoi(arg);
            if (idx > 0) {
                Track t;
                if (playlist_->jump_to_index(idx - 1, t)) {
                    play_track(t, &msg);
                    return;
                } else {
                    reply(msg, tr("Номер трека вне диапазона", "Track number out of range"));
                    return;
                }
            } else {
                reply(msg, tr("Неверный номер", "Invalid track number"));
                return;
            }
        } catch (...) {
            // Not a number, proceed as normal next
        }
    }

    Track next;
    if (get_next_track(next)) {
        play_track(next, &msg);
    } else {
        streamer_->stop();
        TT_DoChangeStatus(tt_, get_gender_mode(), config_.status_message.c_str());
        reply(msg, tr("Выдача закончилась", "No more tracks in playlist"));
    }
}

void Bot::cmd_current(const TextMessage& msg, const std::string& arg) {
    if (!arg.empty()) {
        if (arg == "?") {
            if (streamer_->is_playing() && playlist_->has_tracks()) {
                int cur = playlist_->get_track_index() + 1;
                int tot = static_cast<int>(playlist_->get_track_list().size());
                Track t = streamer_->get_current_track();
                reply(msg, std::to_string(cur) + "/" + std::to_string(tot) + " - " + t.title);
                return;
            }
        }
        try {
            int idx = std::stoi(arg);
            if (idx > 0) {
                Track t;
                if (playlist_->jump_to_index(idx - 1, t)) {
                    play_track(t, &msg);
                    return;
                } else {
                    reply(msg, tr("Номер трека вне списка", "Track number out of range"));
                    return;
                }
            }
        } catch (...) {}
    }

    if (!streamer_->is_playing()) {
        reply(msg, tr("Сейчас ничего не играет", "Nothing is currently playing"));
        return;
    }
    Track t = streamer_->get_current_track();
    int pos = streamer_->get_position();
    int dur = t.duration_sec;
    int cur = playlist_->get_track_index() + 1;
    int tot = static_cast<int>(playlist_->get_track_list().size());

    char buf[1024];
    bool is_en = (config_.language == "en");
    const char* lbl_track = is_en ? "Track" : "Трек";
    const char* lbl_pos = is_en ? "Position" : "Позиция";
    const char* lbl_vol = is_en ? "Volume" : "Громкость";
    const char* lbl_mode = is_en ? "Mode" : "Режим";

    if (tot > 1 && cur > 0) {
        if (dur > 0) {
            snprintf(buf, sizeof(buf), "[%d/%d] %s: %s\n%s: %d:%02d / %d:%02d\n%s: %d%%\n%s: %s",
                     cur, tot, lbl_track, t.title.c_str(), lbl_pos, pos / 60, pos % 60, dur / 60, dur % 60,
                     lbl_vol, streamer_->get_volume(), lbl_mode, playlist_->get_mode_name().c_str());
        } else {
            snprintf(buf, sizeof(buf), "[%d/%d] %s: %s\n%s: %d:%02d\n%s: %d%%\n%s: %s",
                     cur, tot, lbl_track, t.title.c_str(), lbl_pos, pos / 60, pos % 60,
                     lbl_vol, streamer_->get_volume(), lbl_mode, playlist_->get_mode_name().c_str());
        }
    } else if (dur > 0) {
        snprintf(buf, sizeof(buf), "%s: %s\n%s: %d:%02d / %d:%02d\n%s: %d%%\n%s: %s",
                 lbl_track, t.title.c_str(), lbl_pos, pos / 60, pos % 60, dur / 60, dur % 60,
                 lbl_vol, streamer_->get_volume(), lbl_mode, playlist_->get_mode_name().c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s: %s\n%s: %d:%02d\n%s: %d%%\n%s: %s",
                 lbl_track, t.title.c_str(), lbl_pos, pos / 60, pos % 60,
                 lbl_vol, streamer_->get_volume(), lbl_mode, playlist_->get_mode_name().c_str());
    }
    std::string fx_desc = streamer_->get_effects_description(config_.language);
    std::string full_info(buf);
    if (fx_desc != "отключены" && fx_desc != "disabled") {
        full_info += "\n" + this->tr("Эффекты: ", "Effects: ") + fx_desc;
    }
    reply(msg, full_info);
}

void Bot::cmd_seek(const TextMessage& msg, int delta_sec) {
    if (!streamer_->is_playing()) {
        reply(msg, tr("Сейчас ничего не играет", "Nothing is currently playing"));
        return;
    }
    streamer_->seek(delta_sec);
    int pos = streamer_->get_position();
    char buf[256];
    if (config_.language == "en") {
        snprintf(buf, sizeof(buf), "Seeked to %d:%02d", pos / 60, pos % 60);
    } else {
        snprintf(buf, sizeof(buf), "Перемотка на %d:%02d", pos / 60, pos % 60);
    }
    reply(msg, buf);
}

void Bot::cmd_volume(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        int cur_vol = streamer_->get_volume();
        reply(msg, tr("Текущая громкость: ", "Current volume: ") + std::to_string(cur_vol) + "%");
        return;
    }
    try {
        int v = std::stoi(arg);
        v = std::clamp(v, 0, config_.max_volume);
        streamer_->set_volume(v);
        if (db_) db_->set_volume(v);
        reply(msg, tr("Громкость установлена: " + std::to_string(v) + "% (сохранено в БД)",
                     "Volume set to: " + std::to_string(v) + "% (saved to DB)"));
    } catch (...) {
        reply(msg, tr("Использование: v <0-" + std::to_string(config_.max_volume) + ">",
                     "Usage: v <0-" + std::to_string(config_.max_volume) + ">"));
    }
}

void Bot::cmd_speed(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        char buf[256];
        if (config_.language == "en") {
            snprintf(buf, sizeof(buf), "Current speed: %.1fx", streamer_->get_speed());
        } else {
            snprintf(buf, sizeof(buf), "Текущая скорость: %.1fx", streamer_->get_speed());
        }
        reply(msg, buf);
        return;
    }
    try {
        float spd = std::stof(arg);
        spd = std::clamp(spd, 0.5f, 2.0f);
        streamer_->set_speed(spd);
        if (db_) db_->set_speed(spd);
        char buf[256];
        if (config_.language == "en") {
            snprintf(buf, sizeof(buf), "Speed set to: %.1fx", spd);
        } else {
            snprintf(buf, sizeof(buf), "Скорость установлена: %.1fx", spd);
        }
        reply(msg, buf);
    } catch (...) {
        reply(msg, tr("Использование: sp <0.5-2.0>", "Usage: sp <0.5-2.0>"));
    }
}

void Bot::cmd_bass(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        int bb = streamer_->get_bass_boost();
        if (bb > 0) {
            reply(msg, tr("Bass Boost включен: +" + std::to_string(bb) + " dB",
                         "Bass Boost enabled: +" + std::to_string(bb) + " dB"));
        } else {
            reply(msg, tr("Bass Boost отключен. Использование: bb <1-20 / on / off>",
                         "Bass Boost disabled. Usage: bb <1-20 / on / off>"));
        }
        return;
    }
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    if (a == "off" || a == "0") {
        streamer_->set_bass_boost(0);
        if (db_) db_->set_int("fx_bass", 0);
        reply(msg, tr("Bass Boost отключен", "Bass Boost disabled"));
    } else if (a == "on") {
        streamer_->set_bass_boost(10);
        if (db_) db_->set_int("fx_bass", 10);
        reply(msg, tr("Bass Boost включен: +10 dB", "Bass Boost enabled: +10 dB"));
    } else {
        try {
            int lvl = std::stoi(a);
            lvl = std::clamp(lvl, 0, 20);
            streamer_->set_bass_boost(lvl);
            if (db_) db_->set_int("fx_bass", lvl);
            if (lvl > 0) {
                reply(msg, tr("Bass Boost установлен: +" + std::to_string(lvl) + " dB",
                             "Bass Boost set to: +" + std::to_string(lvl) + " dB"));
            } else {
                reply(msg, tr("Bass Boost отключен", "Bass Boost disabled"));
            }
        } catch (...) {
            reply(msg, tr("Использование: bb <1-20 / on / off>", "Usage: bb <1-20 / on / off>"));
        }
    }
}

void Bot::cmd_nightcore(const TextMessage& msg, const std::string& arg) {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    bool enable;
    if (a.empty()) {
        enable = !streamer_->get_nightcore();
    } else if (a == "on" || a == "1" || a == "true") {
        enable = true;
    } else if (a == "off" || a == "0" || a == "false") {
        enable = false;
    } else {
        reply(msg, tr("Использование: nc [on/off]", "Usage: nc [on/off]"));
        return;
    }
    streamer_->set_nightcore(enable);
    if (db_) {
        db_->set_int("fx_nc", enable ? 1 : 0);
        if (enable) db_->set_int("fx_slow", 0);
    }
    reply(msg, enable ? tr("Режим Nightcore включен", "Nightcore mode enabled")
                      : tr("Режим Nightcore отключен", "Nightcore mode disabled"));
}

void Bot::cmd_slowed(const TextMessage& msg, const std::string& arg) {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    bool enable;
    if (a.empty()) {
        enable = !streamer_->get_slowed();
    } else if (a == "on" || a == "1" || a == "true") {
        enable = true;
    } else if (a == "off" || a == "0" || a == "false") {
        enable = false;
    } else {
        reply(msg, tr("Использование: slow [on/off]", "Usage: slow [on/off]"));
        return;
    }
    streamer_->set_slowed(enable);
    if (db_) {
        db_->set_int("fx_slow", enable ? 1 : 0);
        if (enable) db_->set_int("fx_nc", 0);
    }
    reply(msg, enable ? tr("Режим Slowed включен", "Slowed mode enabled")
                      : tr("Режим Slowed отключен", "Slowed mode disabled"));
}

void Bot::cmd_8d(const TextMessage& msg, const std::string& arg) {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    bool enable;
    if (a.empty()) {
        enable = !streamer_->get_8d();
    } else if (a == "on" || a == "1" || a == "true") {
        enable = true;
    } else if (a == "off" || a == "0" || a == "false") {
        enable = false;
    } else {
        reply(msg, tr("Использование: 8d [on/off]", "Usage: 8d [on/off]"));
        return;
    }
    streamer_->set_8d(enable);
    if (db_) db_->set_int("fx_8d", enable ? 1 : 0);
    reply(msg, enable ? tr("Эффект 8D Audio включен", "8D Audio effect enabled")
                      : tr("Эффект 8D Audio отключен", "8D Audio effect disabled"));
}

void Bot::cmd_effects(const TextMessage& msg, const std::string& arg) {
    std::string a = arg;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    if (a == "off" || a == "reset" || a == "clear") {
        streamer_->reset_effects();
        if (db_) {
            db_->set_int("fx_bass", 0);
            db_->set_int("fx_nc", 0);
            db_->set_int("fx_slow", 0);
            db_->set_int("fx_8d", 0);
            db_->set_speed(1.0f);
        }
        reply(msg, tr("Все аудиоэффекты отключены", "All audio effects disabled"));
        return;
    }
    reply(msg, tr("Активные аудиоэффекты: ", "Active audio effects: ") +
               streamer_->get_effects_description(config_.language));
}

void Bot::cmd_mode(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Текущий режим: " + playlist_->get_mode_name() + "\nВарианты: m normal, m single, m all, m random",
                     "Current mode: " + playlist_->get_mode_name() + "\nOptions: m normal, m single, m all, m random"));
        return;
    }
    std::string m = arg;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);
    PlaybackMode pm = PlaybackMode::Normal;
    if (m == "single" || m == "one") {
        pm = PlaybackMode::RepeatOne;
    } else if (m == "all") {
        pm = PlaybackMode::RepeatAll;
    } else if (m == "random" || m == "rand") {
        pm = PlaybackMode::Random;
    }
    playlist_->set_mode(pm);
    if (db_) db_->set_mode(pm);
    reply(msg, tr("Режим воспроизведения: ", "Playback mode: ") + playlist_->get_mode_name());
}

void Bot::cmd_favorites(const TextMessage& msg, const std::string& arg) {
    std::string trimmed = arg;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\r' || trimmed.back() == '\n')) trimmed.pop_back();

    auto favs = db_ ? db_->get_favorites() : playlist_->get_favorites();

    // 1. If empty argument -> list favorites with clear playback hints
    if (trimmed.empty()) {
        if (favs.empty()) {
            reply(msg, tr("Избранное пусто\nЧтобы добавить играющий трек: f +",
                         "Favorites list is empty\nTo add current track: f +"));
            return;
        }
        std::string res = tr("Избранные треки (", "Favorite tracks (") + std::to_string(favs.size()) + "):\n";
        char buf[512];
        for (size_t i = 0; i < favs.size(); ++i) {
            snprintf(buf, sizeof(buf), "%zu: %s\n", i + 1, favs[i].title.c_str());
            res += buf;
        }
        res += tr("\nКак слушать:\nf <номер> — играть трек (например: f 1)\nf all — играть всё подряд\nf + — добавить играющий трек\nf - <номер> — удалить",
                 "\nControls:\nf <index> — play track (e.g. f 1)\nf all — play all\nf + — add current track\nf - <index> — remove");
        reply(msg, res);
        return;
    }

    // 2. f + or f add -> add current track
    if (trimmed == "+" || trimmed == "add") {
        if (!streamer_->is_playing()) {
            reply(msg, tr("Сейчас ничего не играет для добавления в избранное",
                         "Nothing is currently playing to add to favorites"));
            return;
        }
        Track t = streamer_->get_current_track();
        bool ok = db_ ? db_->add_favorite(t) : playlist_->add_favorite(t);
        if (ok) {
            reply(msg, tr("Добавлено в избранное: ", "Added to favorites: ") + t.title);
        } else {
            reply(msg, tr("Этот трек уже есть в избранном", "This track is already in favorites"));
        }
        return;
    }

    // 3. f - <index> or f del <index> -> remove track
    if (trimmed.front() == '-' || trimmed.find("del") == 0 || trimmed.find("remove") == 0) {
        std::string num_str;
        if (trimmed.front() == '-') {
            num_str = trimmed.substr(1);
        } else {
            size_t sp = trimmed.find(' ');
            if (sp != std::string::npos) num_str = trimmed.substr(sp + 1);
        }
        while (!num_str.empty() && num_str.front() == ' ') num_str.erase(num_str.begin());
        try {
            int idx = std::stoi(num_str);
            if (idx > 0 && idx <= static_cast<int>(favs.size())) {
                bool ok = db_ ? db_->remove_favorite(idx - 1) : playlist_->remove_favorite(idx - 1);
                if (ok) {
                    reply(msg, tr("Трек #" + std::to_string(idx) + " удален из избранного",
                                 "Track #" + std::to_string(idx) + " removed from favorites"));
                } else {
                    reply(msg, tr("Ошибка удаления", "Failed to remove track"));
                }
            } else {
                reply(msg, tr("Номер трека вне диапазона списка", "Track number out of range"));
            }
        } catch (...) {
            reply(msg, tr("Использование: f - <номер>", "Usage: f - <index>"));
        }
        return;
    }

    // 4. f clear -> clear all favorites
    if (trimmed == "clear") {
        if (db_) db_->clear_favorites();
        reply(msg, tr("Избранное полностью очищено", "Favorites list cleared"));
        return;
    }

    // 5. f all or f play -> play all favorites as a playlist
    if (trimmed == "all" || trimmed == "play") {
        if (favs.empty()) {
            reply(msg, tr("Избранное пусто", "Favorites list is empty"));
            return;
        }
        playlist_->set_track_list(favs, 0);
        reply(msg, tr("Воспроизведение избранного: " + std::to_string(favs.size()) + " треков",
                     "Playing favorites: " + std::to_string(favs.size()) + " tracks"));
        play_track(favs[0], &msg);
        return;
    }

    // 6. f <index> -> play favorite track by index
    try {
        int idx = std::stoi(trimmed);
        if (idx > 0 && idx <= static_cast<int>(favs.size())) {
            playlist_->set_track_list(favs, idx - 1);
            play_track(favs[idx - 1], &msg);
        } else {
            reply(msg, tr("Номер вне диапазона (от 1 до " + std::to_string(favs.size()) + ")",
                         "Number out of range (1 to " + std::to_string(favs.size()) + ")"));
        }
    } catch (...) {
        reply(msg, tr("Команда не распознана. Используйте:\nf — список\nf <номер> — играть трек\nf all — играть все\nf + — добавить играющий\nf - <номер> — удалить",
                     "Unrecognized command. Use:\nf — list favorites\nf <index> — play track\nf all — play all\nf + — add current track\nf - <index> — delete"));
    }
}

void Bot::cmd_recents(const TextMessage& msg) {
    auto recents = db_ ? db_->get_recents(15) : playlist_->get_recents();
    if (recents.empty()) {
        reply(msg, tr("История воспроизведения пуста", "Playback history is empty"));
        return;
    }
    std::string res = tr("Недавно играли:\n", "Recently played:\n");
    char buf[512];
    for (size_t i = 0; i < recents.size() && i < 15; ++i) {
        snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, recents[i].title.c_str());
        res += buf;
    }
    reply(msg, res);
}

void Bot::cmd_get_link(const TextMessage& msg) {
    if (!streamer_->is_playing()) {
        reply(msg, tr("Сейчас ничего не играет", "Nothing is currently playing"));
        return;
    }
    Track t = streamer_->get_current_track();
    reply(msg, tr("Ссылка на трек:\n", "Track link:\n") + t.webpage_url);
}

void Bot::cmd_queue_add(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Использование: qa <название или ссылка>", "Usage: qa <title or URL>"));
        return;
    }
    reply(msg, tr("Поиск для очереди: ", "Searching for queue: ") + arg + "...");
    Track target;
    if (arg.find("http://") == 0 || arg.find("https://") == 0) {
        if (!resolver_->resolve_direct_url(arg, target)) {
            reply(msg, tr("Ошибка: Не удалось загрузить ссылку", "Error: Failed to resolve URL"));
            return;
        }
    } else {
        auto res = resolver_->search(arg, 1);
        if (res.empty()) {
            reply(msg, tr("Ничего не найдено", "Nothing found"));
            return;
        }
        target = res.front();
    }
    playlist_->add_to_queue(target);
    if (db_) db_->save_queue(playlist_->get_queue());
    reply(msg, tr("Добавлено в очередь: ", "Added to queue: ") + target.title);

    if (!streamer_->is_playing()) {
        Track next;
        if (get_next_track(next)) {
            play_track(next, &msg);
        }
    }
}

void Bot::cmd_queue_list(const TextMessage& msg) {
    auto q = playlist_->get_queue();
    if (q.empty()) {
        reply(msg, tr("Очередь воспроизведения пуста", "Playback queue is empty"));
        return;
    }
    std::string res = tr("Очередь воспроизведения (" + std::to_string(q.size()) + " треков):\n",
                        "Playback queue (" + std::to_string(q.size()) + " tracks):\n");
    char buf[512];
    for (size_t i = 0; i < q.size() && i < 15; ++i) {
        snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, q[i].title.c_str());
        res += buf;
    }
    reply(msg, res);
}

void Bot::cmd_queue_remove(const TextMessage& msg, const std::string& arg) {
    try {
        int idx = std::stoi(arg);
        if (playlist_->remove_from_queue(idx - 1)) {
            if (db_) db_->save_queue(playlist_->get_queue());
            reply(msg, tr("Трек #" + std::to_string(idx) + " удален из очереди",
                         "Track #" + std::to_string(idx) + " removed from queue"));
        } else {
            reply(msg, tr("Неверный номер в очереди", "Invalid queue index"));
        }
    } catch (...) {
        reply(msg, tr("Использование: qr <номер>", "Usage: qr <index>"));
    }
}

void Bot::cmd_queue_clear(const TextMessage& msg) {
    playlist_->clear_queue();
    if (db_) db_->clear_queue();
    reply(msg, tr("Очередь воспроизведения очищена", "Playback queue cleared"));
}

void Bot::cmd_queue_skip(const TextMessage& msg) {
    cmd_next(msg, "");
}

void Bot::cmd_search(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Использование: sr <поисковый запрос>", "Usage: sr <search query>"));
        return;
    }
    reply(msg, tr("Ищу: ", "Searching: ") + arg + "...");
    auto results = resolver_->search(arg, search_limit_);
    if (results.empty()) {
        reply(msg, tr("Ничего не найдено", "Nothing found"));
        return;
    }
    pending_searches_[msg.nFromUserID] = results;

    std::string res = tr("Результаты поиска:\n", "Search results:\n");
    char buf[512];
    for (size_t i = 0; i < results.size(); ++i) {
        snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, results[i].title.c_str());
        res += buf;
    }
    res += tr("Выберите трек командой: sl <номер>", "Select track with command: sl <number>");
    reply(msg, res);
}

void Bot::cmd_select_search(const TextMessage& msg, const std::string& arg) {
    auto it = pending_searches_.find(msg.nFromUserID);
    if (it == pending_searches_.end() || it->second.empty()) {
        reply(msg, tr("У вас нет активных результатов поиска. Сначала выполните: sr <запрос>",
                     "No active search results. First run: sr <query>"));
        return;
    }
    try {
        int idx = std::stoi(arg);
        if (idx >= 1 && idx <= static_cast<int>(it->second.size())) {
            playlist_->set_track_list(it->second, idx - 1);
            play_track(it->second[idx - 1], &msg);
        } else {
            reply(msg, tr("Неверный номер. Введите число от 1 до " + std::to_string(it->second.size()),
                         "Invalid number. Enter number from 1 to " + std::to_string(it->second.size())));
        }
    } catch (...) {
        reply(msg, tr("Использование: sl <номер>", "Usage: sl <number>"));
    }
}

void Bot::cmd_select_search_count(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Текущее количество результатов поиска: ", "Current search results limit: ") +
                   std::to_string(search_limit_));
        return;
    }
    try {
        int c = std::stoi(arg);
        search_limit_ = std::clamp(c, 1, 15);
        if (db_) db_->set_int("search_limit", search_limit_);
        reply(msg, tr("Количество результатов поиска установлено: ", "Search results limit set to: ") +
                   std::to_string(search_limit_));
    } catch (...) {
        reply(msg, tr("Использование: slc <число от 1 до 15>", "Usage: slc <number 1-15>"));
    }
}

void Bot::cmd_join_channel(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        User u{};
        if (msg.nFromUserID > 0 && TT_GetUser(tt_, msg.nFromUserID, &u) && u.nChannelID > 0) {
            config_.channel_path = std::to_string(u.nChannelID);
            if (db_) db_->set_setting("channel_path", config_.channel_path);
            config_.save_to_file();
            TT_DoJoinChannelByID(tt_, u.nChannelID, "");
            reply(msg, tr("Переход в ваш канал ID: ", "Joining your channel ID: ") + std::to_string(u.nChannelID));
            return;
        }
        reply(msg, tr("Использование: jc / come [ID или имя канала] [пароль]",
                     "Usage: jc / come [channel ID or name] [password]"));
        return;
    }

    std::string target = arg;
    std::string password;
    size_t space_pos = arg.find(' ');
    if (space_pos != std::string::npos) {
        target = arg.substr(0, space_pos);
        password = arg.substr(space_pos + 1);
        while (!password.empty() && password.front() == ' ') password.erase(password.begin());
    }

    try {
        int cid = std::stoi(target);
        config_.channel_path = std::to_string(cid);
        config_.channel_password = password;
        if (db_) {
            db_->set_setting("channel_path", config_.channel_path);
            db_->set_setting("channel_password", config_.channel_password);
        }
        config_.save_to_file();
        TT_DoJoinChannelByID(tt_, cid, password.c_str());
        reply(msg, tr("Переход в канал ID: ", "Joining channel ID: ") + std::to_string(cid));
    } catch (...) {
        config_.channel_path = target;
        config_.channel_password = password;
        if (db_) {
            db_->set_setting("channel_path", config_.channel_path);
            db_->set_setting("channel_password", config_.channel_password);
        }
        config_.save_to_file();
        Channel ch{};
        std::strncpy(ch.szName, target.c_str(), sizeof(ch.szName) - 1);
        std::strncpy(ch.szPassword, password.c_str(), sizeof(ch.szPassword) - 1);
        TT_DoJoinChannel(tt_, &ch);
        reply(msg, tr("Переход в канал: ", "Joining channel: ") + target);
    }
}

void Bot::cmd_seek_timecode(const TextMessage& msg, const std::string& arg) {
    if (!streamer_->is_playing()) {
        reply(msg, tr("Сейчас ничего не играет", "Nothing is currently playing"));
        return;
    }
    if (arg.empty()) {
        reply(msg, tr("Использование: seek <ММ:СС> (например: seek 02:15 или seek 90)",
                     "Usage: seek <MM:SS> (e.g.: seek 02:15 or seek 90)"));
        return;
    }
    int total_sec = 0;
    size_t colon1 = arg.find(':');
    if (colon1 != std::string::npos) {
        size_t colon2 = arg.find(':', colon1 + 1);
        try {
            if (colon2 != std::string::npos) {
                int h = std::stoi(arg.substr(0, colon1));
                int m = std::stoi(arg.substr(colon1 + 1, colon2 - colon1 - 1));
                int s = std::stoi(arg.substr(colon2 + 1));
                total_sec = h * 3600 + m * 60 + s;
            } else {
                int m = std::stoi(arg.substr(0, colon1));
                int s = std::stoi(arg.substr(colon1 + 1));
                total_sec = m * 60 + s;
            }
        } catch (...) {
            reply(msg, tr("Неверный формат времени. Пример: seek 02:15",
                         "Invalid time format. Example: seek 02:15"));
            return;
        }
    } else {
        try {
            total_sec = std::stoi(arg);
        } catch (...) {
            reply(msg, tr("Неверный формат времени. Пример: seek 90",
                         "Invalid time format. Example: seek 90"));
            return;
        }
    }
    total_sec = std::max(0, total_sec);
    streamer_->seek_to(total_sec);
    char buf[128];
    if (config_.language == "en") {
        snprintf(buf, sizeof(buf), "Seeked to %02d:%02d", total_sec / 60, total_sec % 60);
    } else {
        snprintf(buf, sizeof(buf), "Перемотка на %02d:%02d", total_sec / 60, total_sec % 60);
    }
    reply(msg, buf);
}

void Bot::cmd_download(const TextMessage& msg, const std::string& arg) {
    Track t;
    if (arg.empty()) {
        t = streamer_->get_current_track();
        if (t.id.empty() && t.stream_url.empty()) {
            reply(msg, tr("Сейчас ничего не играет. Укажите трек: dl <запрос или ссылка>",
                         "Nothing is playing. Specify track: dl <query or URL>"));
            return;
        }
    } else {
        auto res = resolver_->search(arg, 1);
        if (res.empty()) {
            reply(msg, tr("Ничего не найдено для скачивания: ", "Nothing found to download: ") + arg);
            return;
        }
        t = res[0];
    }

    if (t.stream_url.empty()) {
        if (!resolver_->resolve_stream(t)) {
            reply(msg, tr("Ошибка: Не удалось получить поток трека для скачивания",
                         "Error: Failed to obtain stream for download"));
            return;
        }
    }

    int my_chan = TT_GetMyChannelID(tt_);
    if (my_chan <= 0) {
        reply(msg, tr("Ошибка: Бот не находится ни в одном канале",
                     "Error: Bot is not currently in any channel"));
        return;
    }

    reply(msg, tr("Скачиваю и выгружаю в файлы канала: ", "Downloading and uploading to channel files: ") + t.title + "...");

    std::thread([this, t, my_chan, msg]() {
        std::string safe_title = t.title;
        for (char& c : safe_title) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\'') {
                c = '_';
            }
        }
        if (safe_title.empty()) safe_title = "track";
        std::string local_path = "/tmp/" + safe_title + ".mp3";

        std::string cmd = "ffmpeg -y -reconnect 1 -reconnect_streamed 1 -reconnect_at_eof 1 -reconnect_on_network_error 1 -reconnect_delay_max 5 -user_agent \"Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 Version/18.5 Mobile/15E148 Safari/604.1\" -i \"" + t.stream_url + "\" -vn -threads 2 -t 900 -c:a libmp3lame -b:a 192k \"" + local_path + "\" >/dev/null 2>&1";
        LOG_INFO("[Download] Starting ffmpeg conversion for '%s'...", safe_title.c_str());
        std::system(cmd.c_str());

        struct stat st{};
        bool file_ok = (stat(local_path.c_str(), &st) == 0 && st.st_size > 1024);
        LOG_INFO("[Download] ffmpeg done for '%s', exists=%d, size=%lld", local_path.c_str(), file_ok, (long long)st.st_size);

        if (file_ok) {
            INT32 cmd_id = TT_DoSendFile(tt_, my_chan, local_path.c_str());
            LOG_INFO("[Download] TT_DoSendFile returned cmd_id=%d for channel=%d path='%s'", cmd_id, my_chan, local_path.c_str());
            if (cmd_id > 0) {
                reply(msg, this->tr("Файл успешно выгружен в файлы канала (" + std::to_string(my_chan) + "): " + safe_title + ".mp3",
                                  "File uploaded to channel files (" + std::to_string(my_chan) + "): " + safe_title + ".mp3"));
            } else {
                reply(msg, this->tr("Ошибка TeamTalk: Не удалось выгрузить файл в канал (проверьте права на загрузку файлов)",
                                  "TeamTalk Error: Failed to upload file to channel (check upload permissions)"));
            }
        } else {
            reply(msg, this->tr("Ошибка конвертации/скачивания файла: " + t.title,
                              "Error converting/downloading file: " + t.title));
        }
    }).detach();
}

void Bot::cmd_cookies(const TextMessage& msg, const std::string& arg) {
    std::string cookie_path = "cookies.txt";
    if (arg.empty()) {
        std::ifstream f(cookie_path);
        if (!f.is_open()) {
            reply(msg, tr("Файл cookies.txt не найден на сервере.\nИспользование: cookies <содержимое Netscape cookies.txt>",
                         "File cookies.txt not found on server.\nUsage: cookies <Netscape cookies.txt content>"));
            return;
        }
        f.seekg(0, std::ios::end);
        size_t size = f.tellg();
        f.close();
        reply(msg, tr("Файл cookies.txt активен (размер: " + std::to_string(size) + " байт).\nЧтобы перезаписать, отправьте: cookies <текст cookies.txt>",
                     "File cookies.txt is active (size: " + std::to_string(size) + " bytes).\nTo overwrite, send: cookies <cookies.txt content>"));
        return;
    }

    std::ofstream out(cookie_path, std::ios::trunc);
    if (!out.is_open()) {
        reply(msg, tr("Ошибка: Не удалось открыть cookies.txt для записи",
                     "Error: Failed to open cookies.txt for writing"));
        return;
    }
    out << arg << "\n";
    out.close();

    std::ofstream out2("/root/Projects/TTMediaBot/youtube_bridge/cookies.txt", std::ios::trunc);
    if (out2.is_open()) {
        out2 << arg << "\n";
        out2.close();
    }

    reply(msg, tr("Файл cookies.txt успешно сохранён (" + std::to_string(arg.size()) + " байт)!\nСессия моста автоматически подхватит новые куки",
                 "File cookies.txt saved successfully (" + std::to_string(arg.size()) + " bytes)!\nBridge session will automatically reload new cookies"));
}

void Bot::cmd_service(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Текущий музыкальный сервис: " + config_.default_service + "\nВарианты: sv yt, sv ytm",
                     "Current music service: " + config_.default_service + "\nOptions: sv yt, sv ytm"));
        return;
    }
    std::string s = arg;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "yt" || s == "ytm") {
        config_.default_service = s;
        if (db_) db_->set_setting("default_service", s);
        reply(msg, tr("Музыкальный сервис изменен на: ", "Music service changed to: ") + s);
    } else {
        reply(msg, tr("Использование: sv yt или sv ytm", "Usage: sv yt or sv ytm"));
    }
}

void Bot::cmd_help(const TextMessage& msg, const std::string& arg) {
    static const std::unordered_map<std::string, std::string> user_help_ru = {
        {"h", "Показывает справку по командам"},
        {"a", "Показывает информацию о боте"},
        {"p", "ЗАПРОС Воспроизводит треки по запросу. Без запроса — воспроизводит или ставит на паузу"},
        {"u", "URL Воспроизводит поток по заданному URL"},
        {"sv", "СЕРВИС Выбирает сервис для воспроизведения (yt / ytm)"},
        {"s", "Останавливает воспроизведение"},
        {"b", "Воспроизводит предыдущий трек"},
        {"n", "[NUMBER|?] Воспроизводит следующий трек или переходит к треку по номеру"},
        {"c", "[NUMBER|?] Показывает текущую позицию или выбирает трек по номеру"},
        {"sb", "ШАГ Прокручивает текущий трек назад (по умолчанию 10 сек)"},
        {"sf", "ШАГ Прокручивает текущий трек вперед (по умолчанию 10 сек)"},
        {"v", "ГРОМКОСТЬ Устанавливает громкость от 0 до 100"},
        {"sp", "СКОРОСТЬ Устанавливает скорость воспроизведения от 0.5 до 2.0"},
        {"bb", "УРОВЕНЬ Включает усиление басов (Bass Boost) от 0 до 20 дБ"},
        {"nc", "Включает или выключает режим Nightcore"},
        {"slow", "Включает или выключает режим Slowed"},
        {"8d", "Включает или выключает 8D-аудио эффект"},
        {"fx", "Показывает активные аудиоэффекты или сбрасывает их (fx off)"},
        {"f", "+/-НОМЕР Управляет списком избранных треков"},
        {"m", "РЕЖИМ Устанавливает режим воспроизведения (normal, single, all, random)"},
        {"gl", "Получает прямую ссылку на текущий трек"},
        {"r", "НОМЕР Воспроизводит трек из последних треков"},
        {"jc", "Бот заходит в ваш текущий канал (алиасы: come, go)"},
        {"come", "Бот заходит в ваш текущий канал"},
        {"seek", "ММ:СС Перематывает трек на конкретный таймкод"},
        {"dl", "Скачивает трек и выгружает в файлы канала TeamTalk"},
        {"qa", "ЗАПРОС Добавляет трек в очередь"},
        {"ql", "Список всех треков в очереди"},
        {"qr", "NUMBER Удаляет трек из очереди"},
        {"qc", "Очистить всю очередь"},
        {"qs", "Пропускает текущий трек очереди"},
        {"sr", "Режим поиска треков"},
        {"sl", "НОМЕР Выбирает результат поиска"},
        {"slc", "НОМЕР Задаёт количество результатов поиска"}
    };

    static const std::unordered_map<std::string, std::string> admin_help_ru = {
        {"cg", "ПОЛ Меняет пол бота (m/f/n)"},
        {"cl", "ЯЗЫК Меняет язык бота (ru/en)"},
        {"cn", "НИК Меняет ник бота"},
        {"cs", "СТАТУС Меняет статус бота"},
        {"cc", "r/f Очищает кеш бота"},
        {"cm", "Включает или отключает отправку сообщений канала"},
        {"ajc", "Присоединиться к каналу"},
        {"bc", "+/-КОМАНДА Блокирует или разблокирует команды"},
        {"l", "Блокирует или разблокирует бота"},
        {"ua", "+/-ИМЯ Управляет администраторами"},
        {"ub", "+/-ИМЯ Управляет заблокированными пользователями"},
        {"sc", "Сохраняет конфигурацию бота"},
        {"cookies", "ТЕКСТ Просмотр или обновление файла cookies.txt"},
        {"va", "Включает или отключает передачу речи"},
        {"rs", "Перезапускает бота"},
        {"q", "Закрывает бота"},
        {"gcid", "Выводит ID текущего канала"}
    };

    static const std::unordered_map<std::string, std::string> user_help_en = {
        {"h", "Show command help"},
        {"a", "Show bot developer info"},
        {"p", "QUERY Play tracks by query or URL. Without query toggles play/pause"},
        {"u", "URL Play direct audio stream from URL"},
        {"sv", "SERVICE Select music service (yt / ytm)"},
        {"s", "Stop playback and clear current stream"},
        {"b", "Play previous track from history"},
        {"n", "[NUMBER|?] Play next track or jump to track by index"},
        {"c", "[NUMBER|?] Show current track info or jump by index"},
        {"sb", "STEP Rewind backwards (default 10s)"},
        {"sf", "STEP Fast-forward (default 10s)"},
        {"v", "VOLUME Set volume level from 0 to 100"},
        {"sp", "SPEED Set playback speed from 0.5 to 2.0"},
        {"bb", "LEVEL Enable bass boost from 0 to 20 dB"},
        {"nc", "Enable or disable Nightcore effect"},
        {"slow", "Enable or disable Slowed effect"},
        {"8d", "Enable or disable 8D Audio circular effect"},
        {"fx", "Show active audio effects or reset all (fx off)"},
        {"f", "+/-INDEX Manage favorite tracks"},
        {"m", "MODE Set playback mode (normal, single, all, random)"},
        {"gl", "Get web URL of current track"},
        {"r", "INDEX Play track from recent history"},
        {"jc", "Bot joins your current channel (aliases: come, go)"},
        {"come", "Bot joins your current channel"},
        {"seek", "MM:SS Seek track to specific timecode"},
        {"dl", "Download track and upload to channel files"},
        {"qa", "QUERY Enqueue track at the end"},
        {"ql", "List all tracks in queue"},
        {"qr", "NUMBER Remove track by index from queue"},
        {"qc", "Clear entire queue"},
        {"qs", "Skip current queued track"},
        {"sr", "Search mode for tracks"},
        {"sl", "NUMBER Select track from search results"},
        {"slc", "NUMBER Set search results limit"}
    };

    static const std::unordered_map<std::string, std::string> admin_help_en = {
        {"cg", "GENDER Change bot gender (m/f/n)"},
        {"cl", "LANGUAGE Change bot language (ru/en)"},
        {"cn", "NICK Change bot nickname"},
        {"cs", "STATUS Change bot status message"},
        {"cc", "r/f Clear bot cache"},
        {"cm", "Toggle channel messages"},
        {"ajc", "Join channel by name or ID"},
        {"bc", "+/-CMD Block or unblock command"},
        {"l", "Lock or unlock bot (admin only mode)"},
        {"ua", "+/-USER Manage bot administrators"},
        {"ub", "+/-USER Manage banned users list"},
        {"sc", "Save current configuration to file and DB"},
        {"cookies", "TEXT View or update cookies.txt via PM"},
        {"va", "Toggle voice transmission"},
        {"rs", "Restart bot process"},
        {"q", "Shut down bot process"},
        {"gcid", "Print current channel ID"}
    };

    bool is_en = (config_.language == "en");
    const auto& user_help = is_en ? user_help_en : user_help_ru;
    const auto& admin_help = is_en ? admin_help_en : admin_help_ru;

    // Single command help
    if (!arg.empty()) {
        std::string query = arg;
        std::transform(query.begin(), query.end(), query.begin(), ::tolower);
        auto it = user_help.find(query);
        if (it != user_help.end()) {
            reply(msg, it->first + " " + it->second);
            return;
        }
        if (is_admin(msg.szFromUsername)) {
            auto ait = admin_help.find(query);
            if (ait != admin_help.end()) {
                reply(msg, ait->first + " " + ait->second);
                return;
            }
        }
        reply(msg, tr("Неизвестная команда", "Unknown command"));
        return;
    }

    // Full help list
    std::string h = is_en ? "=== Bot Commands ===\n" : "=== Команды бота ===\n";
    for (const auto& kv : user_help) {
        h += kv.first + " " + kv.second + "\n";
    }

    if (is_admin(msg.szFromUsername)) {
        h += is_en ? "\n=== Admin Commands ===\n" : "\n=== Команды администратора ===\n";
        for (const auto& kv : admin_help) {
            h += kv.first + " " + kv.second + "\n";
        }
    }

    reply(msg, h);
}

void Bot::cmd_about(const TextMessage& msg) {
    std::string a = tr(
        "TeamTalk Native Media Bot (C++17 Edition)\n"
        "Разработчик: Фластик\n"
        "Канал: @flastik_blog",
        "TeamTalk Native Media Bot (C++17 Edition)\n"
        "Developer: Flastik\n"
        "Channel: @flastik_blog"
    );
    reply(msg, a);
}

void Bot::cmd_change_nick(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) return;
    config_.nickname = arg;
    if (db_) db_->set_setting("nickname", arg);
    config_.save_to_file();
    TT_DoChangeNickname(tt_, arg.c_str());
    reply(msg, tr("Никнейм изменен на: ", "Nickname changed to: ") + arg);
}

void Bot::cmd_change_status(const TextMessage& msg, const std::string& arg) {
    config_.status_message = arg.empty() ? (config_.language == "en" ? "Send h for help" : "Отправьте h для справки") : arg;
    if (db_) {
        db_->set_status(config_.status_message);
        db_->set_setting("status_message", config_.status_message);
    }
    config_.save_to_file();
    TT_DoChangeStatus(tt_, get_gender_mode(), config_.status_message.c_str());
    reply(msg, tr("Статус изменен на: ", "Status changed to: ") + config_.status_message);
}

void Bot::cmd_change_gender(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Использование: cg <m/f/n>", "Usage: cg <m/f/n>"));
        return;
    }
    char g = std::tolower(arg[0]);
    if (g == 'm' || g == 'f' || g == 'n') {
        config_.gender = std::string(1, g);
        if (db_) db_->set_setting("gender", config_.gender);
        config_.save_to_file();

        std::string current_status = config_.status_message;
        if (streamer_->is_playing()) {
            bool is_en = (config_.language == "en");
            current_status = (streamer_->is_paused() ? (is_en ? "Paused: " : "Пауза: ") : (is_en ? "Playing: " : "Воспроизводится: ")) + streamer_->get_current_track().title;
        }
        TT_DoChangeStatus(tt_, get_gender_mode(), current_status.c_str());

        std::string g_name;
        if (config_.language == "en") {
            g_name = (g == 'f' ? "Female" : (g == 'n' ? "Neutral" : "Male"));
        } else {
            g_name = (g == 'f' ? "Женский" : (g == 'n' ? "Нейтральный" : "Мужской"));
        }
        reply(msg, tr("Пол изменен на: " + config_.gender + " (" + g_name + ")",
                     "Gender changed to: " + config_.gender + " (" + g_name + ")"));
    } else {
        reply(msg, tr("Допустимые значения: m, f, n", "Valid values: m, f, n"));
    }
}

void Bot::cmd_change_language(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, tr("Текущий язык: ", "Current language: ") + config_.language);
        return;
    }
    config_.language = arg;
    if (db_) db_->set_setting("language", arg);
    config_.save_to_file();
    reply(msg, tr("Язык изменен на: ", "Language changed to: ") + arg);
}

void Bot::cmd_voice_toggle(const TextMessage& msg, const std::string& /*arg*/) {
    voice_transmission_enabled_ = !voice_transmission_enabled_;
    if (db_) db_->set_setting("voice_tx", voice_transmission_enabled_ ? "1" : "0");
    bool is_en = (config_.language == "en");
    reply(msg, is_en ? (std::string("Voice transmission: ") + (voice_transmission_enabled_ ? "ON" : "OFF"))
                     : (std::string("Передача голоса: ") + (voice_transmission_enabled_ ? "ВКЛ" : "ВЫКЛ")));
}

void Bot::cmd_channel_msg_toggle(const TextMessage& msg, const std::string& /*arg*/) {
    config_.channel_messages = !config_.channel_messages;
    if (db_) db_->set_setting("channel_messages", config_.channel_messages ? "1" : "0");
    config_.save_to_file();
    bool is_en = (config_.language == "en");
    reply(msg, is_en ? (std::string("Channel messages: ") + (config_.channel_messages ? "ON" : "OFF"))
                     : (std::string("Сообщения в канал: ") + (config_.channel_messages ? "ВКЛ" : "ВЫКЛ")));
}

void Bot::cmd_get_channel_id(const TextMessage& msg) {
    int cid = TT_GetMyChannelID(tt_);
    reply(msg, tr("Текущий ID канала: ", "Current channel ID: ") + std::to_string(cid));
}

void Bot::cmd_block_command(const TextMessage& msg, const std::string& arg) {
    std::istringstream iss(arg);
    std::string action, cmd;
    iss >> action >> cmd;
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (action == "add" && !cmd.empty()) {
        if (db_) db_->block_command(cmd);
        reply(msg, tr("Команда заблокирована: ", "Command blocked: ") + cmd);
    } else if ((action == "del" || action == "remove") && !cmd.empty()) {
        if (db_) db_->unblock_command(cmd);
        reply(msg, tr("Команда разблокирована: ", "Command unblocked: ") + cmd);
    } else {
        auto list = db_ ? db_->get_blocked_commands() : std::vector<std::string>{};
        if (list.empty()) {
            reply(msg, tr("Заблокированных команд нет. Использование: bc add <cmd>, bc del <cmd>",
                         "No blocked commands. Usage: bc add <cmd>, bc del <cmd>"));
        } else {
            std::string s = tr("Заблокированные команды:\n", "Blocked commands:\n");
            for (const auto& c : list) s += "- " + c + "\n";
            reply(msg, s);
        }
    }
}

void Bot::cmd_lock_bot(const TextMessage& msg, const std::string& /*arg*/) {
    locked_ = !locked_;
    if (db_) db_->set_setting("locked", locked_ ? "1" : "0");
    if (config_.language == "en") {
        reply(msg, std::string("Bot ") + (locked_ ? "locked (administrators only)" : "unlocked"));
    } else {
        reply(msg, std::string("Бот ") + (locked_ ? "заблокирован (только для администраторов)" : "разблокирован"));
    }
}

void Bot::cmd_admin_users(const TextMessage& msg, const std::string& arg) {
    std::istringstream iss(arg);
    std::string action, user;
    iss >> action >> user;
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "add" && !user.empty()) {
        if (db_) db_->add_admin_user(user);
        reply(msg, tr("Администратор добавлен: ", "Administrator added: ") + user);
    } else if ((action == "del" || action == "remove") && !user.empty()) {
        if (db_) db_->remove_admin_user(user);
        reply(msg, tr("Администратор удален: ", "Administrator removed: ") + user);
    } else {
        auto list = db_ ? db_->get_admin_users() : config_.admin_users;
        std::string s = tr("Администраторы бота:\n", "Bot administrators:\n");
        for (const auto& u : list) s += "- " + u + "\n";
        s += tr("Использование: ua add <user>, ua del <user>", "Usage: ua add <user>, ua del <user>");
        reply(msg, s);
    }
}

void Bot::cmd_banned_users(const TextMessage& msg, const std::string& arg) {
    std::istringstream iss(arg);
    std::string action, user;
    iss >> action >> user;
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "add" && !user.empty()) {
        if (db_) db_->ban_user(user);
        reply(msg, tr("Пользователь забанен: ", "User banned: ") + user);
    } else if ((action == "del" || action == "remove") && !user.empty()) {
        if (db_) db_->unban_user(user);
        reply(msg, tr("Пользователь разбанен: ", "User unbanned: ") + user);
    } else {
        auto list = db_ ? db_->get_banned_users() : config_.banned_users;
        if (list.empty()) {
            reply(msg, tr("Забаненных пользователей нет. Использование: ub add <user>, ub del <user>",
                         "No banned users. Usage: ub add <user>, ub del <user>"));
        } else {
            std::string s = tr("Забаненные пользователи:\n", "Banned users:\n");
            for (const auto& u : list) s += "- " + u + "\n";
            reply(msg, s);
        }
    }
}

void Bot::cmd_save_config(const TextMessage& msg) {
    if (db_) {
        db_->set_setting("nickname", config_.nickname);
        db_->set_setting("channel_path", config_.channel_path);
        db_->set_setting("gender", config_.gender);
        db_->set_setting("language", config_.language);
        db_->set_setting("status_message", config_.status_message);
        db_->set_status(config_.status_message);
        db_->set_setting("channel_messages", config_.channel_messages ? "1" : "0");
        db_->set_setting("voice_tx", voice_transmission_enabled_ ? "1" : "0");
        db_->set_int("search_limit", search_limit_);
        db_->set_setting("locked", locked_ ? "1" : "0");
        db_->save_queue(playlist_->get_queue());
    }
    if (config_.save_to_file()) {
        reply(msg, tr("Конфигурация успешно сохранена в " + config_.loaded_file_path + " и базу данных",
                     "Configuration successfully saved to " + config_.loaded_file_path + " and database"));
    } else {
        reply(msg, tr("Конфигурация сохранена в базу данных",
                     "Configuration saved to database"));
    }
}

void Bot::cmd_clear_cache(const TextMessage& msg) {
    if (db_) {
        db_->clear_recents();
        db_->clear_queue();
    }
    playlist_->clear_queue();
    reply(msg, tr("История и очередь воспроизведения очищены", "History and playback queue cleared"));
}

#include "bot.hpp"
#include "logger.hpp"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iomanip>

Bot::Bot(BotConfig config)
    : config_(std::move(config)) {}

Bot::~Bot() {
    stop();
}

bool Bot::init() {
    tt_ = TT_InitTeamTalkPoll();
    if (!tt_) {
        LOG_ERR("[Bot] Failed to initialize TeamTalk SDK!");
        return false;
    }

    resolver_ = std::make_unique<YouTubeResolver>(config_.bridge_url);
    streamer_ = std::make_unique<AudioStreamer>(tt_);
    playlist_ = std::make_unique<PlaylistManager>(".");

    streamer_->set_volume(config_.default_volume);
    streamer_->set_on_finished_callback([this]() {
        on_track_finished();
    });

    LOG_INFO("[Bot] Initialized native bot engine successfully.");
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
}

void Bot::connect_and_login() {
    LOG_INFO("[Bot] Connecting to TeamTalk server %s:%d...", config_.host.c_str(), config_.tcp_port);
    TTBOOL ok = TT_Connect(tt_, config_.host.c_str(), config_.tcp_port, config_.udp_port, 0, 0, config_.encrypted ? 1 : 0);
    if (!ok) {
        LOG_ERR("[Bot] TT_Connect failed!");
    }
}

void Bot::process_event(const TTMessage& msg) {
    LOG_INFO("[Event] Received client event: %d", msg.nClientEvent);
    switch (msg.nClientEvent) {
        case CLIENTEVENT_CON_SUCCESS: {
            LOG_INFO("[Bot] Connected to server successfully! Logging in as '%s'...", config_.username.c_str());
            TT_DoLogin(tt_, config_.nickname.c_str(), config_.username.c_str(), config_.password.c_str());
            break;
        }
        case CLIENTEVENT_CON_FAILED: {
            LOG_ERR("[Bot] Connection failed! Reconnecting in 5 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            connect_and_login();
            break;
        }
        case CLIENTEVENT_CON_LOST: {
            LOG_ERR("[Bot] Connection lost! Reconnecting in 3 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            connect_and_login();
            break;
        }
        case CLIENTEVENT_CMD_MYSELF_LOGGEDIN: {
            LOG_INFO("[Bot] Logged in successfully! Joining channel...");
            TT_DoChangeNickname(tt_, config_.nickname.c_str());
            TT_DoChangeStatus(tt_, 0, config_.status_message.c_str());
            
            // Join channel
            if (!config_.channel_path.empty() && config_.channel_path != "/") {
                Channel ch{};
                std::strncpy(ch.szName, config_.channel_path.c_str(), sizeof(ch.szName) - 1);
                std::strncpy(ch.szPassword, config_.channel_password.c_str(), sizeof(ch.szPassword) - 1);
                TT_DoJoinChannel(tt_, &ch);
            } else {
                TT_DoJoinChannelByID(tt_, 1, config_.channel_password.c_str());
            }
            break;
        }
        case CLIENTEVENT_CMD_USER_TEXTMSG: {
            handle_text_message(msg.textmessage);
            break;
        }
        case CLIENTEVENT_CMD_ERROR: {
            LOG_ERR("[Bot] Command error (cmd_id=%d, err_code=%d): %s",
                    msg.nSource, msg.clienterrormsg.nErrorNo, msg.clienterrormsg.szErrorMsg);
            break;
        }
    }
}

void Bot::send_message(int to_user_id, int channel_id, const std::string& text) {
    if (!tt_) return;
    TextMessage tm{};
    if (channel_id > 0) {
        if (!config_.channel_messages) return;
        tm.nMsgType = MSGTYPE_CHANNEL;
        tm.nChannelID = channel_id;
        tm.nToUserID = 0;
    } else {
        tm.nMsgType = MSGTYPE_USER;
        tm.nChannelID = 0;
        tm.nToUserID = to_user_id;
    }
    std::strncpy(tm.szMessage, text.c_str(), sizeof(tm.szMessage) - 1);
    INT32 cmd_id = TT_DoTextMessage(tt_, &tm);
    LOG_INFO("[Chat] TT_DoTextMessage returned cmd_id=%d, chan=%d, user=%d, text: %s", cmd_id, channel_id, to_user_id, text.c_str());
}

void Bot::reply(const TextMessage& original, const std::string& text) {
    if (original.nMsgType == MSGTYPE_CHANNEL) {
        send_message(0, original.nChannelID, text);
    } else {
        send_message(original.nFromUserID, 0, text);
    }
}

bool Bot::is_admin(const std::string& username) const {
    if (config_.admin_users.empty()) return true;
    for (const auto& a : config_.admin_users) {
        if (a == username) return true;
    }
    return false;
}

void Bot::handle_text_message(const TextMessage& msg) {
    LOG_INFO("[Chat] Received text message from '%s' (id=%d, my_id=%d, chan=%d): '%s'",
             msg.szFromUsername, msg.nFromUserID, TT_GetMyUserID(tt_), msg.nChannelID, msg.szMessage);
    // Ignore messages from myself
    if (msg.nFromUserID == TT_GetMyUserID(tt_)) return;

    std::string raw = msg.szMessage;
    // Trim whitespace
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    if (raw.empty()) return;

    // Remove leading prefix if present (e.g. '!' or '/')
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

    // Dispatch commands
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
        int sec = 10;
        try { if (!arg.empty()) sec = std::stoi(arg); } catch (...) {}
        cmd_seek(msg, -sec);
    } else if (cmd == "sf") {
        int sec = 10;
        try { if (!arg.empty()) sec = std::stoi(arg); } catch (...) {}
        cmd_seek(msg, sec);
    } else if (cmd == "v" || cmd == "vol" || cmd == "volume") {
        cmd_volume(msg, arg);
    } else if (cmd == "sp" || cmd == "speed") {
        cmd_speed(msg, arg);
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
    } else if (cmd == "jc") {
        cmd_join_channel(msg, arg);
    } else if (cmd == "h" || cmd == "help") {
        cmd_help(msg, arg);
    } else if (cmd == "a" || cmd == "about") {
        cmd_about(msg);
    }
    // Admin commands
    else if (cmd == "cn") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        cmd_change_nick(msg, arg);
    } else if (cmd == "cs") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        cmd_change_status(msg, arg);
    } else if (cmd == "va") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        cmd_voice_toggle(msg, arg);
    } else if (cmd == "cm") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        cmd_channel_msg_toggle(msg, arg);
    } else if (cmd == "gcid") {
        cmd_get_channel_id(msg);
    } else if (cmd == "q" || cmd == "quit") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        reply(msg, "Завершение работы бота...");
        stop();
    } else if (cmd == "rs" || cmd == "restart") {
        if (!is_admin(user_name)) { reply(msg, "Ошибка: Недостаточно прав админа"); return; }
        reply(msg, "Перезапуск бота...");
        streamer_->stop();
        connect_and_login();
    }
}

void Bot::play_track(const Track& track, const TextMessage* reply_to) {
    Track t = track;
    if (t.stream_url.empty()) {
        if (!resolver_->resolve_stream(t)) {
            if (reply_to) reply(*reply_to, "Ошибка: Не удалось получить аудиопоток для " + t.title);
            return;
        }
    }

    playlist_->add_recent(t);
    streamer_->play(t);

    char buf[1024];
    if (t.duration_sec > 0) {
        snprintf(buf, sizeof(buf), "▶ Сейчас играет: %s [%d:%02d]", t.title.c_str(), t.duration_sec / 60, t.duration_sec % 60);
    } else {
        snprintf(buf, sizeof(buf), "▶ Сейчас играет: %s", t.title.c_str());
    }
    if (reply_to) {
        reply(*reply_to, buf);
    } else {
        send_message(0, TT_GetMyChannelID(tt_), buf);
    }
}

void Bot::on_track_finished() {
    Track next;
    if (playlist_->pop_next(next)) {
        play_track(next);
    }
}

void Bot::cmd_play(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        if (streamer_->is_playing()) {
            if (streamer_->is_paused()) {
                streamer_->resume();
                reply(msg, "Воспроизведение возобновлено");
            } else {
                streamer_->pause();
                reply(msg, "Воспроизведение приостановлено");
            }
            return;
        }

        Track next;
        if (playlist_->pop_next(next)) {
            play_track(next, &msg);
        } else {
            reply(msg, "Очередь пуста. Укажите название трека: p <название>");
        }
        return;
    }

    reply(msg, "Поиск: " + arg + "...");
    Track target;
    if (arg.find("http://") == 0 || arg.find("https://") == 0) {
        if (!resolver_->resolve_direct_url(arg, target)) {
            reply(msg, "Ошибка: Не удалось обработать ссылку");
            return;
        }
    } else {
        auto results = resolver_->search(arg, 1);
        if (results.empty()) {
            reply(msg, "Ничего не найдено по запросу: " + arg);
            return;
        }
        target = results.front();
    }

    play_track(target, &msg);
}

void Bot::cmd_play_url(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Использование: u <ссылка>");
        return;
    }
    reply(msg, "Загрузка потока: " + arg + "...");
    Track t;
    if (resolver_->resolve_direct_url(arg, t)) {
        play_track(t, &msg);
    } else {
        reply(msg, "Ошибка: Не удалось воспроизвести ссылку");
    }
}

void Bot::cmd_stop(const TextMessage& msg) {
    streamer_->stop();
    playlist_->clear_queue();
    reply(msg, "Воспроизведение остановлено, очередь очищена");
}

void Bot::cmd_back(const TextMessage& msg) {
    Track prev;
    if (playlist_->pop_prev(prev)) {
        play_track(prev, &msg);
    } else {
        reply(msg, "История воспроизведения пуста");
    }
}

void Bot::cmd_next(const TextMessage& msg, const std::string& arg) {
    Track next;
    if (playlist_->pop_next(next)) {
        play_track(next, &msg);
    } else {
        streamer_->stop();
        reply(msg, "Очередь закончилась");
    }
}

void Bot::cmd_current(const TextMessage& msg, const std::string& arg) {
    if (!streamer_->is_playing()) {
        reply(msg, "Сейчас ничего не играет");
        return;
    }
    Track t = streamer_->get_current_track();
    int pos = streamer_->get_position();
    int dur = t.duration_sec;

    char buf[1024];
    if (dur > 0) {
        snprintf(buf, sizeof(buf), "Трек: %s\nПозиция: %d:%02d / %d:%02d\nГромкость: %d%%\nРежим: %s",
                 t.title.c_str(), pos / 60, pos % 60, dur / 60, dur % 60,
                 streamer_->get_volume(), playlist_->get_mode_name().c_str());
    } else {
        snprintf(buf, sizeof(buf), "Трек: %s\nПозиция: %d:%02d\nГромкость: %d%%\nРежим: %s",
                 t.title.c_str(), pos / 60, pos % 60,
                 streamer_->get_volume(), playlist_->get_mode_name().c_str());
    }
    reply(msg, buf);
}

void Bot::cmd_seek(const TextMessage& msg, int delta_sec) {
    if (!streamer_->is_playing()) {
        reply(msg, "Сейчас ничего не играет");
        return;
    }
    streamer_->seek(delta_sec);
    int pos = streamer_->get_position();
    char buf[256];
    snprintf(buf, sizeof(buf), "Перемотка на %d:%02d", pos / 60, pos % 60);
    reply(msg, buf);
}

void Bot::cmd_volume(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Текущая громкость: " + std::to_string(streamer_->get_volume()) + "%");
        return;
    }
    try {
        int v = std::stoi(arg);
        streamer_->set_volume(v);
        reply(msg, "Громкость установлена: " + std::to_string(streamer_->get_volume()) + "%");
    } catch (...) {
        reply(msg, "Использование: v <0-100>");
    }
}

void Bot::cmd_speed(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Текущая скорость: %.1fx", streamer_->get_speed());
        reply(msg, buf);
        return;
    }
    try {
        float spd = std::stof(arg);
        streamer_->set_speed(spd);
        char buf[256];
        snprintf(buf, sizeof(buf), "Скорость установлена: %.1fx", streamer_->get_speed());
        reply(msg, buf);
    } catch (...) {
        reply(msg, "Использование: sp <0.5-2.0>");
    }
}

void Bot::cmd_mode(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Текущий режим: " + playlist_->get_mode_name() + "\nВарианты: m normal, m single, m all, m random");
        return;
    }
    std::string m = arg;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);
    if (m == "single" || m == "one") {
        playlist_->set_mode(PlaybackMode::RepeatOne);
    } else if (m == "all") {
        playlist_->set_mode(PlaybackMode::RepeatAll);
    } else if (m == "random" || m == "rand") {
        playlist_->set_mode(PlaybackMode::Random);
    } else {
        playlist_->set_mode(PlaybackMode::Normal);
    }
    reply(msg, "Режим воспроизведения: " + playlist_->get_mode_name());
}

void Bot::cmd_favorites(const TextMessage& msg, const std::string& arg) {
    std::istringstream iss(arg);
    std::string sub;
    iss >> sub;
    std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

    if (sub == "add") {
        if (!streamer_->is_playing()) {
            reply(msg, "Сейчас ничего не играет для добавления в избранное");
            return;
        }
        Track t = streamer_->get_current_track();
        if (playlist_->add_favorite(t)) {
            reply(msg, "Трек добавлен в избранное: " + t.title);
        } else {
            reply(msg, "Трек уже есть в избранном");
        }
    } else if (sub == "del" || sub == "remove") {
        int idx = 0;
        if (iss >> idx && idx > 0) {
            if (playlist_->remove_favorite(idx - 1)) {
                reply(msg, "Трек #" + std::to_string(idx) + " удален из избранного");
            } else {
                reply(msg, "Неверный номер трека");
            }
        } else {
            reply(msg, "Использование: f del <номер>");
        }
    } else {
        auto favs = playlist_->get_favorites();
        if (favs.empty()) {
            reply(msg, "Избранное пусто. Добавить текущий: f add");
            return;
        }
        std::string res = "Избранные треки:\n";
        char buf[512];
        for (size_t i = 0; i < favs.size() && i < 15; ++i) {
            snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, favs[i].title.c_str());
            res += buf;
        }
        reply(msg, res);
    }
}

void Bot::cmd_recents(const TextMessage& msg) {
    auto recents = playlist_->get_recents();
    if (recents.empty()) {
        reply(msg, "История воспроизведения пуста");
        return;
    }
    std::string res = "Недавно играли:\n";
    char buf[512];
    for (size_t i = 0; i < recents.size() && i < 10; ++i) {
        snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, recents[i].title.c_str());
        res += buf;
    }
    reply(msg, res);
}

void Bot::cmd_get_link(const TextMessage& msg) {
    if (!streamer_->is_playing()) {
        reply(msg, "Сейчас ничего не играет");
        return;
    }
    Track t = streamer_->get_current_track();
    reply(msg, "Ссылка на трек:\n" + t.webpage_url);
}

void Bot::cmd_queue_add(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Использование: qa <название или ссылка>");
        return;
    }
    reply(msg, "Поиск для очереди: " + arg + "...");
    Track target;
    if (arg.find("http://") == 0 || arg.find("https://") == 0) {
        if (!resolver_->resolve_direct_url(arg, target)) {
            reply(msg, "Ошибка: Не удалось загрузить ссылку");
            return;
        }
    } else {
        auto res = resolver_->search(arg, 1);
        if (res.empty()) {
            reply(msg, "Ничего не найдено");
            return;
        }
        target = res.front();
    }
    playlist_->add_to_queue(target);
    reply(msg, "Добавлено в очередь: " + target.title);

    if (!streamer_->is_playing()) {
        Track next;
        if (playlist_->pop_next(next)) {
            play_track(next, &msg);
        }
    }
}

void Bot::cmd_queue_list(const TextMessage& msg) {
    auto q = playlist_->get_queue();
    if (q.empty()) {
        reply(msg, "Очередь воспроизведения пуста");
        return;
    }
    std::string res = "Очередь воспроизведения (" + std::to_string(q.size()) + " треков):\n";
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
            reply(msg, "Трек #" + std::to_string(idx) + " удален из очереди");
        } else {
            reply(msg, "Неверный номер в очереди");
        }
    } catch (...) {
        reply(msg, "Использование: qr <номер>");
    }
}

void Bot::cmd_queue_clear(const TextMessage& msg) {
    playlist_->clear_queue();
    reply(msg, "Очередь воспроизведения очищена");
}

void Bot::cmd_queue_skip(const TextMessage& msg) {
    cmd_next(msg, "");
}

void Bot::cmd_search(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Использование: sr <поисковый запрос>");
        return;
    }
    reply(msg, "Ищу: " + arg + "...");
    auto results = resolver_->search(arg, search_limit_);
    if (results.empty()) {
        reply(msg, "Ничего не найдено");
        return;
    }
    pending_searches_[msg.nFromUserID] = results;

    std::string res = "Результаты поиска:\n";
    char buf[512];
    for (size_t i = 0; i < results.size(); ++i) {
        snprintf(buf, sizeof(buf), "%zu. %s\n", i + 1, results[i].title.c_str());
        res += buf;
    }
    res += "Выберите трек командой: sl <номер>";
    reply(msg, res);
}

void Bot::cmd_select_search(const TextMessage& msg, const std::string& arg) {
    auto it = pending_searches_.find(msg.nFromUserID);
    if (it == pending_searches_.end() || it->second.empty()) {
        reply(msg, "У вас нет активных результатов поиска. Сначала вызовите: sr <запрос>");
        return;
    }
    try {
        int idx = std::stoi(arg);
        if (idx >= 1 && idx <= static_cast<int>(it->second.size())) {
            Track selected = it->second[idx - 1];
            play_track(selected, &msg);
        } else {
            reply(msg, "Неверный номер. Введите число от 1 до " + std::to_string(it->second.size()));
        }
    } catch (...) {
        reply(msg, "Использование: sl <номер>");
    }
}

void Bot::cmd_select_search_count(const TextMessage& msg, const std::string& arg) {
    try {
        int c = std::stoi(arg);
        search_limit_ = std::clamp(c, 1, 15);
        reply(msg, "Количество результатов поиска: " + std::to_string(search_limit_));
    } catch (...) {
        reply(msg, "Использование: slc <число от 1 до 15>");
    }
}

void Bot::cmd_join_channel(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) {
        reply(msg, "Использование: jc <путь или ID канала>");
        return;
    }
    try {
        int cid = std::stoi(arg);
        TT_DoJoinChannelByID(tt_, cid, "");
        reply(msg, "Переход в канал ID: " + std::to_string(cid));
    } catch (...) {
        Channel ch{};
        std::strncpy(ch.szName, arg.c_str(), sizeof(ch.szName) - 1);
        TT_DoJoinChannel(tt_, &ch);
        reply(msg, "Переход в канал: " + arg);
    }
}

void Bot::cmd_help(const TextMessage& msg, const std::string& arg) {
    std::string h = 
        "=== Команды TeamTalk Native Media Bot ===\n"
        "p [запрос/ссылка] - играть / пауза / возобновить\n"
        "u <ссылка> - играть прямую ссылку на поток/файл\n"
        "s - остановить воспроизведение\n"
        "n - следующий трек\n"
        "b - предыдущий трек\n"
        "c - текущий трек и статус\n"
        "sb [сек] / sf [сек] - перемотка назад/вперед\n"
        "v [0-100] - громкость звука\n"
        "sp [0.5-2.0] - скорость воспроизведения\n"
        "m [normal/single/all/random] - режим повтора\n"
        "f [add/del/list] - избранные треки\n"
        "r - история последних треков\n"
        "gl - получить ссылку на текущий трек\n"
        "qa <запрос> - добавить в очередь\n"
        "ql - список очереди\n"
        "qr <номер> - удалить из очереди\n"
        "qc - очистить очередь\n"
        "sr <запрос> - расширенный поиск с выбором\n"
        "sl <номер> - выбрать трек из поиска\n"
        "jc <ID> - сменить канал бота\n"
        "a - о создателях бота";
    reply(msg, h);
}

void Bot::cmd_about(const TextMessage& msg) {
    std::string a =
        "=== TeamTalk Native Media Daemon (C++ Edition) ===\n"
        "Автор и главный разработчик: Иван (Ivankolp)\n"
        "Проект: Space Project (@Space_project_fl)\n"
        "Блог: @flastik_blog\n"
        "Движок: Нативный C++20 без PulseAudio и без MPV\n"
        "Прямой инжект звука в движок TeamTalk Opus (48 kHz Stereo)\n"
        "Потребление ОЗУ: ~5-8 МБ | Скорость отклика: < 1 секунды";
    reply(msg, a);
}

void Bot::cmd_change_nick(const TextMessage& msg, const std::string& arg) {
    if (arg.empty()) return;
    config_.nickname = arg;
    TT_DoChangeNickname(tt_, arg.c_str());
    reply(msg, "Никнейм изменен на: " + arg);
}

void Bot::cmd_change_status(const TextMessage& msg, const std::string& arg) {
    config_.status_message = arg;
    TT_DoChangeStatus(tt_, 0, arg.c_str());
    reply(msg, "Статус изменен на: " + arg);
}

void Bot::cmd_voice_toggle(const TextMessage& msg, const std::string& arg) {
    voice_transmission_enabled_ = !voice_transmission_enabled_;
    reply(msg, std::string("Передача голоса: ") + (voice_transmission_enabled_ ? "ВКЛ" : "ВЫКЛ"));
}

void Bot::cmd_channel_msg_toggle(const TextMessage& msg, const std::string& arg) {
    config_.channel_messages = !config_.channel_messages;
    reply(msg, std::string("Сообщения в канал: ") + (config_.channel_messages ? "ВКЛ" : "ВЫКЛ"));
}

void Bot::cmd_get_channel_id(const TextMessage& msg) {
    int cid = TT_GetMyChannelID(tt_);
    reply(msg, "Текущий ID канала: " + std::to_string(cid));
}

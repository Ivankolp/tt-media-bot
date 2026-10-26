#!/usr/bin/env bash
# ==============================================================================
# TeamTalk Native Media Daemon - Complete Automated Installer
# Author: Flastik | @flastik_blog
# ==============================================================================

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
SERVICE_NAME="tt-native-bot.service"
SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}"

echo "================================================================="
echo "  Установка TeamTalk Native Media Bot (C++ Edition by Flastik)   "
echo "================================================================="

if [ "$EUID" -ne 0 ]; then
    echo "[!] Ошибка: скрипт должен быть запущен с правами root (sudo ./install.sh)"
    exit 1
fi

echo "[1/6] Проверка и установка системных пакетов..."
if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y
    apt-get install -y build-essential libsqlite3-dev libcurl4-openssl-dev ffmpeg yt-dlp nodejs npm git curl nlohmann-json3-dev
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y gcc-c++ make sqlite-devel libcurl-devel ffmpeg yt-dlp nodejs npm git curl json-devel
elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm base-devel sqlite curl ffmpeg yt-dlp nodejs npm git nlohmann-json
fi

echo "[2/6] Настройка окружения библиотек TeamTalk..."
mkdir -p "${DIR}/lib"
if [ -f "${DIR}/lib/libTeamTalk5.so" ] && [ -s "${DIR}/lib/libTeamTalk5.so" ]; then
    echo "[*] Библиотека libTeamTalk5.so найдена в репозитории"
else
    echo "[*] Загрузка официальной библиотеки TeamTalk 5 SDK (x86_64)..."
    curl -fsSL -o "${DIR}/lib/libTeamTalk5.so" "https://raw.githubusercontent.com/Ivankolp/tt-media-bot/main/lib/libTeamTalk5.so" 2>/dev/null || true
    if [ ! -f "${DIR}/lib/libTeamTalk5.so" ] || [ ! -s "${DIR}/lib/libTeamTalk5.so" ]; then
        for fallback in "/opt/teamtalk/libTeamTalk5.so" "/usr/local/lib/libTeamTalk5.so" "/usr/lib/libTeamTalk5.so"; do
            if [ -f "$fallback" ]; then
                cp "$fallback" "${DIR}/lib/libTeamTalk5.so"
                echo "[*] Скопирована локальная библиотека из $fallback"
                break
            fi
        done
    fi
fi

echo "[3/6] Настройка турбо-моста YouTube (PO-токены и кэш)..."
if [ -d "${DIR}/bridge" ]; then
    cd "${DIR}/bridge"
    if [ ! -d "node_modules" ]; then
        echo "[*] Установка зависимостей youtubei.js..."
        npm install --production --silent 2>/dev/null || true
    fi
    cd "${DIR}"
fi

# Настройка генератора PO-токенов bgutil (если еще не стоит)
if [ ! -d "/opt/bgutil-provider" ]; then
    echo "[*] Установка генератора токенов bgutil в /opt/bgutil-provider..."
    git clone https://github.com/Brainicism/bgutil-ytdlp-pot-provider.git /opt/bgutil-provider 2>/dev/null || true
    if [ -d "/opt/bgutil-provider/server" ]; then
        cd /opt/bgutil-provider/server
        npm install --silent 2>/dev/null || true
        npm run build --silent 2>/dev/null || true
        cd "${DIR}"
    fi
fi

# Регистрация bgutil сервиса
if [ -f "/opt/bgutil-provider/server/build/main.js" ] && [ ! -f "/etc/systemd/system/bgutil-pot.service" ]; then
    cat <<EOF > /etc/systemd/system/bgutil-pot.service
[Unit]
Description=YouTube PO Token Provider (bgutil)
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/bgutil-provider/server
Environment=NODE_OPTIONS="--max-old-space-size=96"
ExecStart=/usr/bin/node /opt/bgutil-provider/server/build/main.js --port 4416 --host 127.0.0.1
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable bgutil-pot.service
    systemctl restart bgutil-pot.service || true
fi

# Регистрация сервиса YouTube-моста
if [ -f "${DIR}/bridge/server.mjs" ]; then
    cat <<EOF > /etc/systemd/system/tt-bridge.service
[Unit]
Description=TeamTalk Native Bot YouTube Bridge
After=network.target bgutil-pot.service
Wants=bgutil-pot.service

[Service]
Type=simple
User=root
WorkingDirectory=${DIR}/bridge
Environment=NODE_ENV=production
Environment=YOUTUBE_BRIDGE_HOST=127.0.0.1
Environment=YOUTUBE_BRIDGE_PORT=4417
Environment=NODE_OPTIONS="--max-old-space-size=96"
ExecStart=/usr/bin/node ${DIR}/bridge/server.mjs
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable tt-bridge.service
    systemctl restart tt-bridge.service || true
fi

echo "[4/6] Компиляция C++ бота..."
cd "${DIR}"
make clean
make -j"$(nproc)"
strip "${DIR}/tt-media-bot"

echo "[5/6] Проверка конфига..."
if [ ! -f "${DIR}/config.json" ]; then
    if [ -f "${DIR}/config.example.json" ]; then
        cp "${DIR}/config.example.json" "${DIR}/config.json"
        echo "[*] Создан config.json из образца"
    fi
fi

echo "[6/6] Создание и запуск systemd сервиса..."
cat <<EOF > "${SERVICE_PATH}"
[Unit]
Description=TeamTalk Native Media Daemon (C++ Edition by Flastik)
After=network.target teamtalk.service tt-bridge.service
Wants=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${DIR}
ExecStart=${DIR}/run.sh
Restart=on-failure
RestartPreventExitStatus=0
RestartForceExitStatus=42
RestartSec=1s
LimitNOFILE=65535
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# Регистрация шаблонного сервиса для мультиботов
cat <<EOF > /etc/systemd/system/tt-bot@.service
[Unit]
Description=TeamTalk Native Media Bot - Instance %i (C++ Edition by Flastik)
After=network.target teamtalk.service tt-bridge.service
Wants=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${DIR}
ExecStart=${DIR}/run.sh configs/%i.json
Restart=on-failure
RestartPreventExitStatus=0
RestartForceExitStatus=42
RestartSec=1s
LimitNOFILE=65535
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

chmod +x "${DIR}/bot-ctl" "${DIR}/run.sh"
mkdir -p "${DIR}/configs" "${DIR}/data"

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"
systemctl restart "${SERVICE_NAME}"

echo ""
echo "================================================================="
echo "  [ГОТОВО] Бот успешно скомпилирован, установлен и запущен!      "
echo "  Основной бот: systemctl status ${SERVICE_NAME}                 "
echo "  Мультиботы:   ./bot-ctl status                                 "
echo "  Добавить бота: ./bot-ctl add <имя>                             "
echo "================================================================="

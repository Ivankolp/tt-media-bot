> [!NOTE]
> **Русскоязычным пользователям:** Документация на русском языке доступна в файле [README_RU.md](README_RU.md)

# TeamTalk Native Media Bot (C++ Edition)

High-performance native media bot for TeamTalk 5 written in C++17.  
Plays audio from YouTube and direct streams, supports audio effects, playback queues, favorites, and channel file uploads.

**Author:** Flastik  
**Telegram Blog:** [@flastik_blog](https://t.me/flastik_blog)  
**Russian Documentation:** [README_RU.md](README_RU.md)

---

## Installation & Setup

### Requirements:
- Linux x86_64 or ARM64
- C++17 compiler (`g++`) and `make`
- `ffmpeg`
- `libcurl4-openssl-dev`, `libsqlite3-dev`, `nlohmann-json3-dev`
- `Node.js` v18+

### 1. Build
```bash
make -j$(nproc)
```

### 2. Configuration
Copy the configuration template:
```bash
cp config.example.json config.json
```

Edit your TeamTalk server credentials in `config.json`:
```json
{
    "config_version": 0,
    "general": {
        "language": "ru",
        "send_channel_messages": false,
        "cache_file_name": "bot.db",
        "blocked_commands": [],
        "delete_uploaded_files_after": 300,
        "time_format": "%H:%M",
        "status": "Send h for help",
        "start_commands": []
    },
    "player": {
        "default_volume": 25,
        "max_volume": 100,
        "volume_fading": false,
        "volume_fading_interval": 0.005,
        "seek_step": 10
    },
    "teamtalk": {
        "hostname": "127.0.0.1",
        "tcp_port": 16815,
        "udp_port": 16815,
        "encrypted": false,
        "nickname": "Media Bot",
        "status": "Send h for help",
        "gender": "n",
        "username": "bot_native",
        "password": "your_password",
        "channel": "/",
        "channel_password": "",
        "reconnection_attempts": -1,
        "reconnection_timeout": 5,
        "users": {
            "admins": [
                "admin"
            ],
            "banned_users": []
        }
    },
    "services": {
        "default_service": "yt",
        "bridge_url": "http://127.0.0.1:4417",
        "yt": {
            "enabled": true,
            "search_results": 1
        },
        "ytm": {
            "enabled": true,
            "search_results": 1
        }
    }
}
```

### 3. Launch
```bash
./run.sh
```

### 4. Automatic Systemd Installation
To automatically install dependencies, set up background services, and compile:
```bash
sudo ./install.sh
```

---

## Commands

Commands can be sent in private messages to the bot. Supported prefixes: `!`, `/`, or without prefix.

### Playback & Controls:
| Command | Arguments | Description |
|---|---|---|
| `p` | `[query / url]` | Play, pause, or resume track |
| `u` | `<stream url>` | Play direct audio stream, web radio, or file URL |
| `s` | - | Stop playback |
| `n` | - | Next track in queue or playlist |
| `b` | - | Previous track from history |
| `c` | - | Current track info, elapsed time, total duration, volume |
| `seek` | `<MM:SS>` | Seek to specific timecode (e.g. `seek 02:45`) |
| `st` | `<seconds>` | Seek to exact position in seconds (e.g. `st 120`) |
| `sb` | `[seconds]` | Rewind (default: 10 seconds) |
| `sf` | `[seconds]` | Fast-forward (default: 10 seconds) |
| `v` | `[0-100]` | Set volume level |
| `sp` | `[0.5-2.0]` | Playback speed |
| `bb` | `[1-20 / on / off]` | Bass Boost (0-20 dB) |
| `nc` | `[on / off]` | Nightcore mode (pitch up, speed 1.25x) |
| `slow` | `[on / off]` | Slowed mode (pitch down, tempo 0.85x) |
| `8d` | `[on / off]` | 8D audio circular stereo effect |
| `fx` | `[off / reset]` | Show active audio effects or reset them |
| `dl` | `[query / url]` | Download track to MP3 and upload to channel files |
| `m` | `[normal / single / all / random]` | Playback loop mode |
| `f` | - | List favorite tracks |
| `f +` / `f add` | - | Add current track to favorites |
| `f - <id>` | `<id>` | Remove track from favorites |
| `r` | - | Recent tracks history |
| `gl` | - | Get web URL of the current playing track |

### Queue:
| Command | Arguments | Description |
|---|---|---|
| `qa` | `<query / url>` | Add track to the end of queue |
| `ql` | - | View current queue |
| `qr` | `<index>` | Remove track from queue by number |
| `qc` | - | Clear queue |
| `qs` | - | Skip current queued track |

### Search:
| Command | Arguments | Description |
|---|---|---|
| `sr` | `<query>` | Search tracks by query |
| `sl` | `<index>` | Play track from search results |
| `slc` | `<1-15>` | Set number of search results to display |

### Channel & Navigation:
| Command | Arguments | Description |
|---|---|---|
| `come` / `go` / `jc` | - | Bot joins your current channel |
| `jc` | `<ID / name> [pwd]` | Join specified channel |
| `sv` | `[yt / ytm]` | Switch default music service |
| `h` | - | Show command help |
| `a` | - | About developer |

### Admin Commands:
| Command | Arguments | Description |
|---|---|---|
| `cookies` | `[content]` | View or update `cookies.txt` via PM |
| `cn` | `<name>` | Change bot nickname |
| `cs` | `<status>` | Change bot status message |
| `cg` | `<m / f / n>` | Change bot gender |
| `cl` | `<ru / en>` | Change bot language setting |
| `cm` | - | Toggle channel public announcements |
| `va` | - | Toggle voice transmission mode |
| `bc` | `[add / del / list]` | Manage blocked commands for regular users |
| `ua` | `[add / del / list]` | Manage bot administrators |
| `ub` | `[add / del / list]` | Manage banned users list |
| `l` | - | Lock bot (admins only) |
| `sc` | - | Save current configuration to `config.json` |
| `cc` | - | Clear SQLite cache and history |
| `gcid` | - | Get current channel ID |
| `rs` | - | Restart bot process |
| `q` | - | Shut down bot process |

---

## Author
Developed by: **Flastik**  
Telegram: [@flastik_blog](https://t.me/flastik_blog)

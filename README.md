# Smart Alarm - ESP32-CAM + Telegram

A smart alarm clock that requires you to scan a QR code to prove you're awake!

## How It Works

1. **Set alarm via Telegram**: Send `/set 07:30` to schedule your alarm
2. **Alarm triggers**: When it's time, your ESP32-CAM starts buzzing and the bot sends a QR code to your phone
3. **Scan to stop**: Hold your phone up to the camera - it scans the QR code and stops the alarm
4. **Track your progress**: See how long it takes you to wake up with `/stats`

## Architecture

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   Telegram App  │◄────│   Python Server │◄────│   ESP32-CAM     │
│   (Your Phone)  │     │   (Your Laptop) │     │   (The Alarm)   │
└─────────────────┘     └─────────────────┘     └─────────────────┘
        │                        │                       │
        │  /set 07:30            │                       │
        │───────────────────────►│                       │
        │                        │  Poll every 2s        │
        │                        │◄──────────────────────│
        │                        │                       │
        │                        │  At 07:30: state=RING │
        │                        │──────────────────────►│
        │                        │                       │ 🔊
        │  📷 QR Code            │                       │
        │◄───────────────────────│                       │
        │                        │  Send JPEG frames     │
        │                        │◄──────────────────────│
        │                        │                       │
        │                        │  QR valid → STOP      │
        │                        │──────────────────────►│
        │  ☀️ Good morning!      │                       │ 🔇
        │◄───────────────────────│                       │
```

## Setup

### 1. Create Telegram Bot

1. Open Telegram and search for `@BotFather`
2. Send `/newbot` and follow the prompts
3. Copy the **Bot Token**

### 2. Configure Server

```bash
cd server

# Copy example config
cp ../.env.example ../.env

# Edit .env with your values
```

Edit `.env`:
```env
TELEGRAM_BOT_TOKEN=your_bot_token_here
DEVICE_CALLBACK_URL=http://YOUR_LAPTOP_IP:8000
```

Find your laptop's IP:
- Windows: `ipconfig` (look for IPv4 Address)
- Mac/Linux: `ifconfig` or `ip addr`

### 3. Install Python Dependencies

```bash
cd server
python -m venv venv

# Windows
.\venv\Scripts\activate

# Mac/Linux
source venv/bin/activate

pip install -r requirements.txt
```

**Note**: `pyzbar` requires the ZBar library:
- **Windows**: Download from https://sourceforge.net/projects/zbar/files/zbar/0.10/zbar-0.10-setup.exe
- **Mac**: `brew install zbar`
- **Linux**: `sudo apt-get install libzbar0`

### 4. Configure ESP32-CAM

Edit `device_code.ino`:
```cpp
const char* WIFI_SSID = "Your_WiFi_Name";
const char* WIFI_PASSWORD = "Your_WiFi_Password";
const char* SERVER_URL = "http://YOUR_LAPTOP_IP:8000";
```

### 5. Upload to ESP32-CAM

1. Open `device_code.ino` in Arduino IDE
2. Select Board: `AI Thinker ESP32-CAM`
3. Connect FTDI programmer
4. Upload

### 6. Run Server

```bash
cd server
.\venv\Scripts\activate  # or source venv/bin/activate
python main.py
```

## Telegram Commands

| Command | Description |
|---------|-------------|
| `/start` | Register & see help |
| `/set HH:MM` | Set alarm (e.g., `/set 07:30`) |
| `/status` | Check current alarm |
| `/cancel` | Cancel scheduled alarm |
| `/stats` | View wake-up statistics |
| `/test` | Trigger alarm immediately (for testing) |

## Troubleshooting

### ESP32 can't connect to server
- Make sure laptop and ESP32 are on the same WiFi network
- Check firewall isn't blocking port 8000
- Verify SERVER_URL in ESP32 code matches laptop IP

### QR code not scanning
- Ensure good lighting
- Hold phone steady, about 20-30cm from camera
- Check server logs for "QR decode" messages

### No Telegram messages
- Verify TELEGRAM_BOT_TOKEN is correct
- Register with the bot first by sending `/start`

## Project Structure

```
smart_alarm/
├── device_code.ino    # ESP32-CAM firmware
├── .env               # Your configuration (create from .env.example)
├── .env.example       # Configuration template
├── README.md          # This file
└── server/
    ├── main.py            # FastAPI server entry point
    ├── config.py          # Configuration loader
    ├── database.py        # SQLite models
    ├── alarm_manager.py   # Alarm state machine
    ├── qr_handler.py      # QR generation/decoding
    ├── telegram_bot.py    # Telegram commands
    └── requirements.txt   # Python dependencies
```

## License

MIT

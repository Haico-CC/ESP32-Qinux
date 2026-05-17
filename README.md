# ESP32-Qinux 🐧📟

A lightweight, web-based Linux-inspired terminal shell for ESP32S3 / ESP32, featuring local file management, hardware control, a built-in scripting engine, and **on-device SLM inference** running natively on PSRAM.

## ✨ Features
- 🌐 **Web Terminal**: Responsive CRT-style UI with real-time WebSocket communication.
- 📁 **File System**: Complete LittleFS management (`ls`, `cd`, `cat`, `edit`, `cp`, `mv`, `rm -r`, `grep`, `echo >`, etc.)
- ⚡ **Hardware Control**: Safe GPIO read/write, ADC1 voltage sampling, and WiFi status/tx-power management.
- 🤖 **On-Device SLM**: Run Transformer models (e.g., `stories260K`) directly on ESP32. Supports streaming token output.
- 📜 **Scripting Engine**: Variables, arithmetic, `if/else`, `for`/`while` loops, `sleep`, `break`/`continue`, and file input redirection.
- 📥📤 **Chunked Transfer**: Reliable WebSocket-based file download/upload (up to 5MB, 4KB chunks with ACK & timeout).
- 🎮 **Easter Eggs**: Matrix rain (`matrix`), falling cats (`cat`), and fortune cookies (`fortune`).
- 🛡️ **Safety**: Protected directories (`/bin`, `/etc`, `/sys`), automatic GPIO/ADC validation, and WDT-safe `yield()` loops.

## 🛠️ Requirements & Setup
- **Hardware**: ESP32-S3 / ESP32 (PSRAM **strongly recommended** for SLM)
- **Software**: Arduino IDE or PlatformIO with ESP32 Arduino Core (v2.x or v3.x)
- **Libraries**: Built-in (`WiFi`, `WebServer`, `WebSocketsServer`, `DNSServer`, `LittleFS`)
- **SLM Models**: `stories260K.bin` (or compatible) + `tok512.bin` (must be placed in LittleFS `/bin/`)

### Quick Start
1. Compile & flash the project to your ESP32.
2. Upload the SLM model & tokenizer to LittleFS (use Arduino LittleFS uploader or `mkspiffs`).
3. Power on the board. It automatically creates a Wi-Fi AP:
   - **SSID**: `Welcome Back to the 80's !`
   - **Password**: (none)
   - **IP**: `192.168.4.1`
4. Open any browser and visit `http://192.168.4.1` to access the terminal.

## 📟 Command Reference
| Command | Description |
|---|---|
| `help` | Show all available commands |
| `wifi status` / `scan` | View WiFi info, signal quality & scan networks |
| `gpio -s <pin> <0/1>` | Set GPIO level (auto-validates safe pins) |
| `adc [pin]` | Read ADC1 raw value & voltage |
| `dl <file>` / `ul` | Download / Upload files (chunked WebSocket) |
| `edit <file>` | Interactive line editor (type `EOF` to save & exit) |
| `run <script>` | Execute a shell script file |
| `clear` / `reset` | Clear terminal / show last reset reason |

## 🤖 SLM Integration
```bash
# Load default model from /bin/
llama init

# Load custom model (relative paths)
llama init models/my.bin tokens/tok.bin

# Generate text (prompt MUST be quoted)
llama "Hello ESP32!" -l 128

# Check status or free PSRAM
llama status
llama free
```
> 💡 **Note**: SLM generation runs **synchronously** in the WebSocket handler. The terminal will be blocked until generation completes. Requires PSRAM for weight storage. Models must follow the `llama2.c` binary checkpoint format.

## 📜 Scripting Engine
Write scripts and run them via `run script.sh`. Syntax is POSIX-inspired but lightweight:
```sh
set greeting="Hello"
set count=3
for i in 1..$count do
  echo "$greeting #$i"
  sleep 1
done

if $? == 0 then
  echo "Loop finished successfully."
else
  echo "Something went wrong."
fi

read user_input
echo "You typed: $user_input"
```
**Supported:** `set`, `read`, `if/else`, `for`/`while`, `break`, `continue`, `$?` (last exit code), `#` comments, arithmetic `(+ - * / %)` in `set`.

## ⚠️ Notes & Limitations
- **SLM Speed**: ~20 tok/s on ESP32S3 N16R8 (depends on CPU clock & PSRAM bandwidth).
- **Transfer Limit**: Max file size **5MB** (limited by RAM & chunk timeout).
- **WiFi Safety**: Commands that change SSID/channel/mode are **disabled** to prevent WebSocket drops.
- **Protected Paths**: `/bin`, `/etc`, `/sys` are read-only. Use `/home` or root for user files.
- **Core Compatibility**: WiFi TX power & sleep APIs are auto-adapted for Arduino Core 2.x & 3.x.

## 📄 License
This project is licensed under the **GNU General Public License v3.0 (GPLv3)**.
Forks, contributions, and hardware integrations are highly encouraged, provided they strictly uphold the principles of software freedom and source code transparency. 🛠️
#ifndef ESP_COMMANDS_H
#define ESP_COMMANDS_H

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <map>
#include <time.h>

// ========== 全局对象 extern ==========
extern String currentPath, serialInputBuffer;
extern bool isEditing;
extern String editingFilePath;
extern int g_lastExitCode;
extern uint8_t llmActiveClient;
extern bool llmGenerationActive;
extern WebSocketsServer webSocket;

// ========== Fortune 彩蛋库 ==========
static const char* FORTUNES[] PROGMEM = {
  "🔮 The cake is a lie.", "🔮 sudo make me a sandwich", "🔮 Everything is... 42.",
  "🔮 Have you tried turning it off and on again?", "🔮 It works on my machine.",
  "🔮 There's no place like 127.0.0.1", "🔮 To err is human, to really foul things up requires root.",
  "🔮 I'm not lazy, I'm in energy-saving mode.", "🔮 Hardware: The part of a computer that can be kicked.",
  "🔮 All your base are belong to us.", "🔮 Talk is cheap. Show me the code. - Linus Torvalds",
  "🔮 First, solve the problem. Then, write the code. - John Johnson",
  "🔮 ESP32: Because sometimes you need two cores to handle the chaos.",
  "🔮 Debugging: Being the detective in a crime movie where you are also the murderer.",
  "🔮 Coffee: The official programming language of ESP32 developers.",
  "🔮 Warning: May contain traces of undefined behavior.",
  "🔮 If at first you don't succeed; call it version 1.0.",
  "🔮 I would agree with you, but then we'd both be wrong.",
  "🔮 My code doesn't have bugs, it just develops random features.",
  "🔮 Keep calm and rm -rf /"
};
const int FORTUNE_COUNT = sizeof(FORTUNES) / sizeof(FORTUNES[0]);

// ========== 核心命令执行 ==========
void executeCommand(String cmd, String& output, String& newPrompt, bool& clearTerminal, String& dlFileName, String& dlContent, bool& triggerUpload) {
  output = "";
  newPrompt = "root@esp32:" + currentPath + "# ";
  clearTerminal = false; dlFileName = ""; dlContent = ""; triggerUpload = false;
  g_lastExitCode = 0;

  // 串口逐行编辑模式
  if (isEditing) {
    newPrompt = "";
    if (cmd == "EOF") {
      isEditing = false; newPrompt = "root@esp32:" + currentPath + "# ";
      output = "\nedit: File saved: " + editingFilePath + "\n"; editingFilePath = "";
      return;
    }
    if (!canWritePath(editingFilePath)) {
      output = "edit: Permission denied\n"; isEditing = false;
      newPrompt = "root@esp32:" + currentPath + "# "; editingFilePath = "";
      return;
    }
    File f = LittleFS.open(editingFilePath, "a");
    if (f) { f.println(cmd); f.close(); output = ""; }
    else { output = "edit: Error: Failed to write file!\n"; isEditing = false; newPrompt = "root@esp32:" + currentPath + "# "; editingFilePath = ""; }
    return;
  }

  if (cmd.startsWith("__SYNC_TIME__:")) { setSystemTime(cmd.substring(14).toInt()); output = "time: Browser time applied to ESP32 RTC.\n"; return; }

  // ========== llama 命令 ==========
  if (cmd == "llama init" || cmd.startsWith("llama init ")) {
    String modelPath = LLM_MODEL_PATH, tokenPath = LLM_TOKEN_PATH;
    if (cmd.startsWith("llama init ")) {
      String args = cmd.substring(10); args.trim();
      int spacePos = args.indexOf(' ');
      if (spacePos == -1) { output = "llama: Error: init requires 2 arguments\nllama: Usage: llama init <model.bin> <tokens.bin>\n"; return; }
      modelPath = args.substring(0, spacePos); tokenPath = args.substring(spacePos + 1);
      modelPath.trim(); tokenPath.trim();
      if (!modelPath.startsWith("/")) modelPath = resolvePath(modelPath);
      if (!tokenPath.startsWith("/")) tokenPath = resolvePath(tokenPath);
    }
    if (!LittleFS.exists(modelPath)) { output = "llama: Model not found: " + modelPath + "\n"; return; }
    if (!LittleFS.exists(tokenPath)) { output = "llama: Token file not found: " + tokenPath + "\n"; return; }
    if (llm_bridge_init(modelPath.c_str(), tokenPath.c_str()))
      output = "llama: Model loaded: " + modelPath + "\nllama: Tokens: " + tokenPath + "\n";
    else output = "llama: Init failed. Check serial logs.\n";
    return;
  }
  if (cmd == "llama status") {
    if (!llm_bridge_is_ready()) output = "llama: Status: Not initialized\n";
    else if (llm_bridge_is_busy()) output = "llama: Status: Generating...\n";
    else output = "llama: Status: Ready\n";
    return;
  }
  if (cmd == "llama free") {
    if (!llm_bridge_is_ready()) { output = "llama: Not initialized\n"; return; }
    llm_bridge_free(); output = "llama: Model unloaded. PSRAM freed.\n";
    return;
  }
  if (cmd.startsWith("llama ")) {
    String args = cmd.substring(6); args.trim();
    if (!llm_bridge_is_ready()) { output = "llama: Run 'llama init' first\n"; return; }
    if (llm_bridge_is_busy()) { output = "llama: Busy generating.\n"; return; }
    String prompt = ""; bool hasQuotes = false; int promptEnd = -1;
    if (args.length() >= 2) {
      if (args.startsWith("\"")) {
        for (int i = 1; i < args.length(); i++) { if (args[i] == '"' && (i == 1 || args[i-1] != '\\')) { prompt = args.substring(1, i); promptEnd = i + 1; hasQuotes = true; break; } }
      } else if (args.startsWith("'")) {
        for (int i = 1; i < args.length(); i++) { if (args[i] == '\'' && (i == 1 || args[i-1] != '\\')) { prompt = args.substring(1, i); promptEnd = i + 1; hasQuotes = true; break; } }
      }
    }
    if (!hasQuotes) { output = "llama: Error: Prompt must be enclosed in quotes\nllama: Usage: llama \"<prompt>\" [-l N]\n"; return; }
    int max_len = 256;
    if (promptEnd >= 0 && promptEnd < args.length()) {
      String rest = args.substring(promptEnd); rest.trim();
      int lPos = rest.indexOf("-l "); if (lPos == -1) lPos = rest.indexOf("-l");
      if (lPos != -1) { String numStr = ""; for (int i = lPos+2; i < rest.length() && isdigit(rest[i]); i++) numStr += rest[i]; int parsed = numStr.toInt(); if (parsed > 0 && parsed <= 2048) max_len = parsed; }
    }
    if (!prompt.isEmpty() && prompt.charAt(prompt.length()-1) != ' ') prompt += " ";
    if (prompt.isEmpty()) { output = "llama: Error: Empty prompt\n"; return; }
    llm_bridge_generate(prompt.c_str(), max_len, on_llm_token, on_llm_done, &llmActiveClient);
    return;
  }

  // ========== help ==========
  if (cmd == "help" || cmd.startsWith("help ")) {
    String topic = (cmd == "help") ? "" : cmd.substring(5); topic.trim();

    // ---- 详细帮助 ----
    if (topic == "ls") {
      output += "ls — List directory contents\n";
      output += "  ls [path]      List files/directories at path\n";
      output += "  ls -l [path]   Detailed view (type, size, name)\n";
      output += "  Directories are shown with trailing /\n"; return;
    }
    if (topic == "cd") {
      output += "cd — Change working directory\n";
      output += "  cd <path>      Change to target directory\n";
      output += "  cd /           Go to root\n";
      output += "  cd ..          Go up one level\n";
      output += "  cd             Same as cd /\n"; return;
    }
    if (topic == "pwd") { output += "pwd — Print current working directory path\n"; return; }
    if (topic == "cat") {
      output += "cat — Display file content\n";
      output += "  cat <file>     Print file contents to terminal\n";
      output += "  cat            Easter egg! 🐱\n"; return;
    }
    if (topic == "touch") {
      output += "touch — Create an empty file\n";
      output += "  touch <file>   Create file if not exists; update timestamp\n"; return;
    }
    if (topic == "mkdir") {
      output += "mkdir — Create a directory\n";
      output += "  mkdir <dir>    Create new directory (parents must exist)\n"; return;
    }
    if (topic == "rm") {
      output += "rm — Remove files/directories\n";
      output += "  rm <file>      Delete a file\n";
      output += "  rm -r <dir>    Recursively delete directory and contents\n";
      output += "  Protected dirs (/bin, /etc, /sys) cannot be removed\n"; return;
    }
    if (topic == "cp") {
      output += "cp — Copy files\n";
      output += "  cp <src> <dst>   Copy source file to destination\n"; return;
    }
    if (topic == "mv") {
      output += "mv — Move / rename files and directories\n";
      output += "  mv <src> <dst>   Move or rename source to destination\n"; return;
    }
    if (topic == "echo") {
      output += "echo — Print text or write to file\n";
      output += "  echo <text>              Print text to terminal\n";
      output += "  echo \"text\" > file        Overwrite file\n";
      output += "  echo \"text\" >> file       Append to file\n"; return;
    }
    if (topic == "grep") {
      output += "grep — Search file for pattern\n";
      output += "  grep <pattern> <file>    Print matching lines with line numbers\n"; return;
    }
    if (topic == "edit") {
      output += "edit — Edit files\n";
      output += "  edit <file>     Open file in terminal line editor or web editor\n";
      output += "  Type 'EOF' on a new line to save & exit (serial mode)\n";
      output += "  Web editor: Ctrl+S to save, Esc to cancel\n"; return;
    }
    if (topic == "run") {
      output += "run — Execute a shell script\n";
      output += "  run <script>              Run script file\n";
      output += "  run <script> <input>      Run with input data file\n";
      output += "  Scripts support: if/elif/else/fi, for/while, set, read, sleep\n"; return;
    }
    if (topic == "set") {
      output += "set — Define or calculate variables (script only)\n";
      output += "  set var=value     Assign value to variable\n";
      output += "  set var=$a + $b   Arithmetic: + - * / % ( ) supported\n";
      output += "  set var=\"text\"    String value (use quotes)\n";
      output += "  Access with $var  in later commands\n"; return;
    }
    if (topic == "if") {
      output += "if — Conditional execution (script only)\n";
      output += "  if <cond>\n";
      output += "    ...\n";
      output += "  elif <cond>\n";
      output += "    ...\n";
      output += "  else\n";
      output += "    ...\n";
      output += "  fi\n";
      output += "  Operators: == != < > <= >=\n";
      output += "  Example: if $x >= 10\n"; return;
    }
    if (topic == "for") {
      output += "for — Numeric loop (script only)\n";
      output += "  for <var> in <start>..<end> do\n";
      output += "    ...\n";
      output += "  done\n";
      output += "  Supports both ascending and descending ranges.\n";
      output += "  Example: for i in 1..5 do ... done\n";
      output += "           for i in 5..1 do ... done\n"; return;
    }
    if (topic == "while") {
      output += "while — Conditional loop (script only)\n";
      output += "  while <cond> do\n";
      output += "    ...\n";
      output += "  done\n";
      output += "  Max 500 iterations. Use break to exit early.\n";
      output += "  Example: while $cnt > 0 do ... set cnt=$cnt - 1 ... done\n"; return;
    }
    if (topic == "sleep") {
      output += "sleep — Delay execution\n";
      output += "  sleep 2         Wait 2 seconds\n";
      output += "  sleep 500ms     Wait 500 milliseconds\n";
      output += "  In script, yields between delay chunks.\n"; return;
    }
    if (topic == "read") {
      output += "read — Read input into variable (script only)\n";
      output += "  read <var>      Read next input line into $var\n"; return;
    }
    if (topic == "break" || topic == "continue") {
      output += topic + " — Loop control (script only)\n";
      output += "  " + topic + "           Exit/skip current loop iteration\n"; return;
    }
    if (topic == "wifi") {
      output += "wifi — WiFi management (AP+STA dual mode)\n";
      output += "  wifi                      Show status summary\n";
      output += "  wifi info                 Full configuration details\n";
      output += "  wifi scan                 List nearby networks (RSSI, CH, AUTH)\n";
      output += "  wifi stats                Signal quality and rates\n";
      output += "  wifi connect <SSID> [PW]  Connect to network\n";
      output += "  wifi connect \"WiFi Name\"  SSID with spaces needs quotes\n";
      output += "  wifi disconnect            Disconnect (keeps saved config)\n";
      output += "  wifi forget                Clear saved WiFi credentials\n";
      output += "  wifi set power <dBm>       Adjust TX power (2~20)\n";
      output += "  Credentials saved to /sys/wifi.cfg, auto-connect on boot\n"; return;
    }
    if (topic == "date") {
      output += "date — Show or set system time\n";
      output += "  date                    Show current date/time\n";
      output += "  date -s \"YYYY-MM-DD HH:MM:SS\"   Set time\n"; return;
    }
    if (topic == "sysinfo") {
      output += "sysinfo — Comprehensive system information\n";
      output += "  Shows: chip model, CPU freq, heap/PSRAM, flash, SDK, WiFi IP, reset reason\n"; return;
    }
    if (topic == "df") {
      output += "df — Disk usage\n";
      output += "  Shows LittleFS total / used / free bytes\n"; return;
    }
    if (topic == "free") {
      output += "free — Memory usage\n";
      output += "  Shows free heap and free PSRAM\n"; return;
    }
    if (topic == "uname") { output += "uname — System name and version\n"; return; }
    if (topic == "cpuinfo") {
      output += "cpuinfo — CPU details\n";
      output += "  Shows: core count, frequency, SRAM, PSRAM\n"; return;
    }
    if (topic == "gpio") {
      output += "gpio — GPIO control\n";
      output += "  gpio                      Show all safe GPIO states\n";
      output += "  gpio <pin>                Read pin state (HIGH/LOW)\n";
      output += "  gpio -s <pin> <mode>      Set: low|high=OUT  in=IN  up=PU  down=PD\n";
      output += "  Safe pins: " + getSafeGpioList() + "\n"; return;
    }
    if (topic == "adc") {
      output += "adc — Analog read (ADC1)\n";
      output += "  adc <pin>      Read voltage (pin required)\n";
      output += "  Valid pins: " + getAdc1ValidPins() + "\n"; return;
    }
    if (topic == "dl") {
      output += "dl — Download file from ESP32\n";
      output += "  dl <file>      Start chunked download (max 5 MB)\n";
      output += "  File is saved via browser download dialog\n"; return;
    }
    if (topic == "ul") {
      output += "ul — Upload file to ESP32\n";
      output += "  ul             Opens browser file picker\n";
      output += "  Supports chunked upload up to 5 MB\n"; return;
    }
    if (topic == "llama" || topic == "llm") {
      output += "llama — Local LLM inference\n";
      output += "  llama init                        Load default model\n";
      output += "  llama init <model> <tokenizer>    Load custom model\n";
      output += "  llama \"<prompt>\" [-l N]           Generate text (max N tokens)\n";
      output += "  llama status                      Show model status\n";
      output += "  llama free                        Unload model\n";
      output += "  Prompt MUST be quoted. Example: llama \"Hello world\" -l 100\n"; return;
    }
    if (topic == "clear" || topic == "reset") {
      output += topic + " — Clear terminal screen\n"; return;
    }
    if (topic == "matrix") {
      output += "matrix — Digital rain easter egg 🌧️\n";
      output += "  Press any key to exit.\n"; return;
    }
    if (topic == "fortune") {
      output += "fortune — Ask the silicon oracle 🎱\n"; return;
    }
    if (!topic.isEmpty()) {
      output += "help: No help for '" + topic + "'\n";
      output += "Type 'help' without arguments for command list.\n"; return;
    }

    // ---- 概览 ----
    output += "=== ESP32-Qinux Help ===\n";
    output += "Type 'help <cmd>' for detailed usage.\n";
    output += " NAVIGATION    ls  cd  pwd\n";
    output += " FILES         cat  touch  mkdir  rm  cp  mv  echo  grep  edit\n";
    output += " SCRIPTING     run  set  if  for  while  sleep  read  break  continue\n";
    output += " WIFI          wifi (status scan connect disconnect forget)\n";
    output += " SYSTEM        uname  cpuinfo  free  df  sysinfo  date  reset\n";
    output += " LLM           llama init / \"prompt\" / status / free\n";
    output += " HARDWARE      gpio  adc\n";
    output += " TRANSFER      dl  ul\n";
    output += " FUN           clear  matrix  fortune  cat\n";
    return;
  }

  // ========== 文件系统 / 系统命令 ==========
  if (cmd == "dl" || cmd.startsWith("dl ")) {
    String pathArg = (cmd == "dl") ? "" : cmd.substring(3); pathArg.trim();
    if (pathArg.isEmpty()) { output = "dl: Usage: dl <file>\n"; return; }
    String fullPath = resolvePath(pathArg);
    if (!LittleFS.exists(fullPath)) { output = "dl: " + pathArg + ": No such file\n"; return; }
    File f = LittleFS.open(fullPath, "r");
    if (f.isDirectory()) { output = "dl: " + pathArg + ": Is a directory\n"; f.close(); return; }
    f.close();
    if (!startChunkedDownload(fullPath, llmActiveClient)) output = "dl: Failed to start transfer\n";
    return;
  }
  if (cmd == "ul") { triggerUpload = true; output = "ul: Please select a file via browser popup...\n"; return; }

  if (cmd == "run" || cmd.startsWith("run ")) {
    String args = (cmd == "run") ? "" : cmd.substring(4); args.trim();
    int spacePos = args.indexOf(" "); String scriptPath = args, inputPath = "";
    if (spacePos != -1) { scriptPath = args.substring(0, spacePos); inputPath = args.substring(spacePos + 1); scriptPath.trim(); inputPath.trim(); }
    if (scriptPath.isEmpty()) { output = "run: Usage: run <script> [input_file]\n"; return; }
    String fullPath = resolvePath(scriptPath);
    if (!LittleFS.exists(fullPath)) { output = "run: File not found: " + scriptPath + "\n"; return; }
    File f = LittleFS.open(fullPath, "r");
    if (!f || f.isDirectory()) { output = "run: Not a valid file\n"; f.close(); return; }
    std::vector<String> lines;
    String line;
    while (f.available()) { char c = f.read(); if (c == '\n') { lines.push_back(line); line = ""; } else if (c != '\r') line += c; }
    if (!line.isEmpty()) lines.push_back(line);
    f.close();
    if (!inputPath.isEmpty()) {
      loadScriptInput(resolvePath(inputPath));
      scriptInputIndex = 0;
    }
    scriptVars.clear();
    output += "---------------------------------\n";
    output += "run: Executing: " + fullPath + "\n";
    output += "---------------------------------\n";
    executeScriptBlockEx(lines, 0, lines.size(), output);
    output += "---------------------------------\n";
    output += "run: Completed!\n";
    output += "---------------------------------\n";
    return;
  }

  if (cmd == "ls" || cmd.startsWith("ls ")) {
    String args = (cmd == "ls") ? "" : cmd.substring(3); args.trim();
    bool showDetail = (args == "-l");
    String targetPath = showDetail ? currentPath : (args.isEmpty() ? currentPath : resolvePath(args));
    File dir = LittleFS.open(targetPath, "r");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); output = "ls: Not a directory\n"; return; }
    int count = 0;
    while (true) {
      File entry = dir.openNextFile();
      if (!entry) break;
      count++;
      if (showDetail) {
        output += entry.isDirectory() ? "d " : "- ";
        output += String(entry.size()) + "\t";
        output += entry.name();
      } else output += String(entry.name()) + (entry.isDirectory() ? "/" : "");
      output += "\n";
      entry.close();
    }
    dir.close();
    if (count == 0) output = "(empty)\n";
    return;
  }

  if (cmd == "pwd") { output = currentPath + "\n"; return; }

  if (cmd == "cd" || cmd.startsWith("cd ")) {
    String target = (cmd == "cd") ? "/" : cmd.substring(3); target.trim();
    if (target.isEmpty()) { currentPath = "/"; return; }
    String newPath = resolvePath(target);
    if (!LittleFS.exists(newPath)) { output = "cd: " + target + ": No such directory\n"; return; }
    File d = LittleFS.open(newPath, "r");
    if (!d || !d.isDirectory()) { if (d) d.close(); output = "cd: " + target + ": Not a directory\n"; return; }
    d.close();
    currentPath = newPath;
    return;
  }

  if (cmd == "cat" || cmd.startsWith("cat ")) {
    if (cmd == "cat") { output = "__CAT_EASTER_EGG__"; return; }
    String pathArg = cmd.substring(4); pathArg.trim();
    if (pathArg.isEmpty()) { output = "cat: Usage: cat <file>\n"; return; }
    String fullPath = resolvePath(pathArg);
    if (LittleFS.exists(fullPath)) {
      File f = LittleFS.open(fullPath, "r");
      if (f && !f.isDirectory()) { while (f.available()) output += (char)f.read(); output += "\n"; }
      else output = "cat: " + pathArg + ": Is a directory\n";
      f.close();
    } else output = "cat: " + pathArg + ": No such file\n";
    return;
  }

  if (cmd == "edit" || cmd.startsWith("edit ")) {
    String pathArg = (cmd == "edit") ? "" : cmd.substring(5); pathArg.trim();
    if (pathArg.isEmpty()) { output = "edit: Usage: edit <file>\n"; return; }
    if (!canWritePath(resolvePath(pathArg))) { output = "edit: Permission denied\n"; return; }
    editingFilePath = resolvePath(pathArg);
    File f = LittleFS.open(editingFilePath, "w");
    if (!f) { output = "edit: Cannot create file\n"; editingFilePath = ""; return; }
    output = "edit: " + pathArg + " - Type 'EOF' on a new line to save & exit\n";
    if (LittleFS.exists(editingFilePath)) {
      File existing = LittleFS.open(editingFilePath, "r");
      if (existing && !existing.isDirectory()) { while (existing.available()) output += (char)existing.read(); existing.close(); }
    }
    isEditing = true; f.close();
    return;
  }

  if (cmd == "mkdir" || cmd.startsWith("mkdir ")) {
    String pathArg = (cmd == "mkdir") ? "" : cmd.substring(6); pathArg.trim();
    if (pathArg.isEmpty()) { output = "mkdir: Usage: mkdir <dir>\n"; return; }
    String fullPath = resolvePath(pathArg);
    if (isProtectedPath(fullPath)) { output = "mkdir: Permission denied\n"; return; }
    if (LittleFS.mkdir(fullPath)) output = "mkdir: Created " + pathArg + "\n";
    else output = "mkdir: Failed\n";
    return;
  }

  if (cmd == "touch" || cmd.startsWith("touch ")) {
    String pathArg = (cmd == "touch") ? "" : cmd.substring(6); pathArg.trim();
    if (pathArg.isEmpty()) { output = "touch: Usage: touch <file>\n"; return; }
    String fullPath = resolvePath(pathArg);
    if (!canWritePath(fullPath)) { output = "touch: Permission denied\n"; return; }
    File f = LittleFS.open(fullPath, "a");
    if (f) { f.close(); output = "touch: Created " + pathArg + "\n"; }
    else output = "touch: Failed\n";
    return;
  }

  if (cmd == "rm" || cmd.startsWith("rm ")) {
    String args = (cmd == "rm") ? "" : cmd.substring(3); args.trim();
    bool recursive = false; String pathArg = args;
    if (args.startsWith("-r ")) { recursive = true; pathArg = args.substring(3); }
    else if (args == "-r") { output = "rm: Usage: rm [-r] <path>\n"; return; }
    pathArg.trim();
    if (pathArg.isEmpty()) { output = "rm: Usage: rm [-r] <path>\n"; return; }
    String fullPath = resolvePath(pathArg);
    if (isProtectedPath(fullPath)) { output = "rm: Permission denied\n"; return; }
    if (!LittleFS.exists(fullPath)) { output = "rm: " + pathArg + ": No such file\n"; return; }
    File f = LittleFS.open(fullPath, "r");
    if (f && f.isDirectory()) {
      f.close();
      if (!recursive) { output = "rm: " + pathArg + ": Is a directory (use -r)\n"; return; }
      if (deleteDirectoryRecursive(fullPath)) output = "rm: Removed directory " + pathArg + "\n";
      else output = "rm: Failed\n";
    } else {
      f.close();
      if (LittleFS.remove(fullPath)) output = "rm: Removed " + pathArg + "\n";
      else output = "rm: Failed\n";
    }
    return;
  }

  if (cmd == "cp" || cmd.startsWith("cp ")) {
    String args = (cmd == "cp") ? "" : cmd.substring(3); args.trim();
    int spacePos = args.indexOf(' ');
    if (spacePos == -1) { output = "cp: Usage: cp <src> <dst>\n"; return; }
    String src = args.substring(0, spacePos), dst = args.substring(spacePos + 1);
    src.trim(); dst.trim();
    String srcPath = resolvePath(src), dstPath = resolvePath(dst);
    if (!LittleFS.exists(srcPath)) { output = "cp: " + src + ": No such file\n"; return; }
    File srcFile = LittleFS.open(srcPath, "r");
    if (srcFile.isDirectory()) { output = "cp: " + src + ": Is a directory (not supported)\n"; srcFile.close(); return; }

    // 如果目标是已存在的目录，则保留原文件名放入该目录
    File dstCheck = LittleFS.open(dstPath, "r");
    if (dstCheck && dstCheck.isDirectory()) {
      dstCheck.close();
      String srcName = srcPath.substring(srcPath.lastIndexOf('/') + 1);
      dstPath = dstPath + (dstPath.endsWith("/") ? "" : "/") + srcName;
    } else if (dstCheck) dstCheck.close();

    File dstFile = LittleFS.open(dstPath, "w");
    if (!dstFile) { output = "cp: Cannot write to " + dst + "\n"; srcFile.close(); return; }
    while (srcFile.available()) dstFile.write(srcFile.read());
    srcFile.close(); dstFile.close();
    output = "cp: Copied " + src + " -> " + dstPath + "\n";
    return;
  }

  if (cmd == "mv" || cmd.startsWith("mv ")) {
    String args = (cmd == "mv") ? "" : cmd.substring(3); args.trim();
    int spacePos = args.indexOf(' ');
    if (spacePos == -1) { output = "mv: Usage: mv <src> <dst>\n"; return; }
    String src = args.substring(0, spacePos), dst = args.substring(spacePos + 1);
    src.trim(); dst.trim();
    String srcPath = resolvePath(src), dstPath = resolvePath(dst);
    if (!LittleFS.exists(srcPath)) { output = "mv: " + src + ": No such file\n"; return; }
    if (isProtectedPath(srcPath)) { output = "mv: Permission denied\n"; return; }
    File srcFile = LittleFS.open(srcPath, "r");
    if (!srcFile) { output = "mv: Cannot open source\n"; return; }
    if (srcFile.isDirectory()) {
      srcFile.close();
      if (LittleFS.rename(srcPath, dstPath)) output = "mv: Moved " + src + " -> " + dst + "\n";
      else output = "mv: Failed\n";
      return;
    }
    File dstFile = LittleFS.open(dstPath, "w");
    if (!dstFile) { output = "mv: Cannot write to " + dst + "\n"; srcFile.close(); return; }
    while (srcFile.available()) dstFile.write(srcFile.read());
    srcFile.close(); dstFile.close();
    LittleFS.remove(srcPath);
    output = "mv: Moved " + src + " -> " + dst + "\n";
    return;
  }

  if (cmd == "echo" || cmd.startsWith("echo ")) {
    if (cmd == "echo") { output = "echo: Usage: echo <text> [> file] [>> file]\n"; return; }
    String rest = cmd.substring(5); rest.trim();
    int gtPos = -1, gtgtPos = -1;
    bool inQuote = false; char quoteChar = 0;
    for (int i = 0; i < rest.length(); i++) {
      char c = rest[i];
      if ((c == '"' || c == '\'') && (i == 0 || rest[i-1] != '\\')) {
        if (!inQuote) { inQuote = true; quoteChar = c; } else if (c == quoteChar) inQuote = false;
      } else if (!inQuote) {
        if (c == '>' && i+1 < rest.length() && rest[i+1] == '>') { gtgtPos = i; break; }
        else if (c == '>' && gtPos == -1) gtPos = i;
      }
    }
    String text = "", filePath = ""; bool append = false;
    if (gtgtPos != -1) { text = rest.substring(0, gtgtPos); filePath = rest.substring(gtgtPos+2); append = true; }
    else if (gtPos != -1) { text = rest.substring(0, gtPos); filePath = rest.substring(gtPos+1); }
    else text = rest;
    text.trim(); filePath.trim();
    if (text.length() >= 2 && ((text.startsWith("\"") && text.endsWith("\"")) || (text.startsWith("'") && text.endsWith("'")))) text = text.substring(1, text.length()-1);
    if (!filePath.isEmpty()) {
      if (!canWritePath(resolvePath(filePath))) { output = "echo: Permission denied\n"; return; }
      File f = LittleFS.open(resolvePath(filePath), append ? "a" : "w");
      if (f) { f.println(text); f.close(); output = "echo: Written to " + filePath + "\n"; }
      else output = "echo: Cannot write to file\n";
    } else output = text + "\n";
    return;
  }

  if (cmd == "grep" || cmd.startsWith("grep ")) {
    String args = (cmd == "grep") ? "" : cmd.substring(5); args.trim();
    int spacePos = args.indexOf(' ');
    if (spacePos == -1) { output = "grep: Usage: grep <pattern> <file>\n"; return; }
    String pattern = args.substring(0, spacePos), fileArg = args.substring(spacePos + 1);
    pattern.trim(); fileArg.trim();
    String fullPath = resolvePath(fileArg);
    if (!LittleFS.exists(fullPath)) { output = "grep: " + fileArg + ": No such file\n"; return; }
    File f = LittleFS.open(fullPath, "r");
    if (!f || f.isDirectory()) { if (f) f.close(); output = "grep: Not a valid file\n"; return; }
    int lineNum = 0, matchCount = 0;
    while (f.available()) {
      String l = f.readStringUntil('\n'); lineNum++;
      if (l.indexOf(pattern) != -1) { output += String(lineNum) + ": " + l + "\n"; matchCount++; }
    }
    f.close();
    if (matchCount == 0) output = "grep: No matches for '" + pattern + "'\n";
    return;
  }

  if (cmd == "wifi" || cmd.startsWith("wifi ")) {
    String args = (cmd == "wifi") ? "" : cmd.substring(5); args.trim();
    handleWifiCommand(args, output);
    return;
  }

  if (cmd == "uname") { output = "ESP32-Qinux 1.0 (esp32)\n"; return; }
  if (cmd == "cpuinfo") {
    output += "CPU: Xtensa dual-core 32-bit LX6\nCores: 2\nFrequency: 240 MHz\n";
    output += "SRAM: 520 KB\nPSRAM: " + String(ESP.getPsramSize()/1024/1024) + " MB\n";
    return;
  }
  if (cmd == "free") {
    output += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    output += "Free PSRAM: " + String(ESP.getFreePsram()) + " bytes\n";
    return;
  }
  if (cmd == "df") {
    output += "Total: " + String(LittleFS.totalBytes()) + " bytes\n";
    output += "Used:  " + String(LittleFS.usedBytes()) + " bytes\n";
    output += "Free:  " + String(LittleFS.totalBytes() - LittleFS.usedBytes()) + " bytes\n";
    return;
  }
  if (cmd == "sysinfo") {
    output += "System: ESP32-Qinux 1.0\nChip: " + String(ESP.getChipModel()) + " Rev " + String(ESP.getChipRevision()) + "\n";
    output += "CPU: " + String(ESP.getCpuFreqMHz()) + " MHz\n";
    output += "Heap: " + String(ESP.getFreeHeap()) + " free / " + String(ESP.getHeapSize()) + " total\n";
    output += "PSRAM: " + String(ESP.getFreePsram()) + " free / " + String(ESP.getPsramSize()) + " total\n";
    output += "Flash: " + String(ESP.getFlashChipSize()/1024/1024) + " MB\n";
    output += "SDK: " + String(ESP.getSdkVersion()) + "\n";
    output += "WiFi IP: " + WiFi.softAPIP().toString() + "\n";
    output += "Reset: " + getResetReason() + "\n";
    return;
  }
  if (cmd == "date" || cmd.startsWith("date ")) {
    String args = (cmd == "date") ? "" : cmd.substring(5); args.trim();
    if (args.startsWith("-s ")) {
      String dateStr = args.substring(3); dateStr.trim();
      time_t t;
      if (parseDateTimeString(dateStr, t)) { setSystemTime(t); output = "date: Set to " + dateStr + "\n"; }
      else output = "date: Invalid format. Use: YYYY-MM-DD HH:MM:SS\n";
    } else {
      time_t now; time(&now);
      struct tm* tm = localtime(&now);
      char buf[30]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
      output = String(buf) + "\n";
    }
    return;
  }
  if (cmd == "reset") { clearTerminal = true; return; }
  if (cmd == "clear") { clearTerminal = true; return; }

  if (cmd == "fortune") { output = FORTUNES[random(FORTUNE_COUNT)] + String("\n"); return; }
  if (cmd == "matrix") { output = "__MATRIX_MODE__"; return; }

  // ========== GPIO ==========
  if (cmd == "gpio" || cmd.startsWith("gpio ")) {
    String args = (cmd == "gpio") ? "" : cmd.substring(5); args.trim();
    if (args.startsWith("-s ")) {
      String rest = args.substring(3); rest.trim();
      // 解析: gpio -s <pin> <mode>
      int sp = rest.indexOf(' ');
      if (sp == -1) {
        output = "gpio: Usage: gpio -s <pin> <low|high|in|up|down>\n";
        output += "  low/high  Output LOW / HIGH\n";
        output += "  in       Input (floating)\n";
        output += "  up       Input + pull-up\n";
        output += "  down     Input + pull-down\n";
        return;
      }
      int pin = rest.substring(0, sp).toInt();
      String modeArg = rest.substring(sp + 1); modeArg.trim();
      if (!isSafeGpio(pin)) { output = "gpio: GPIO" + String(pin) + " is not safe\nSafe: " + getSafeGpioList() + "\n"; return; }

      if (modeArg == "low" || modeArg == "high") {
        bool level = (modeArg == "high");
        pinMode(pin, OUTPUT);
        digitalWrite(pin, level ? HIGH : LOW);
        output = "gpio: GPIO" + String(pin) + " -> OUTPUT " + (level ? "HIGH" : "LOW") + "\n";
      } else if (modeArg == "in") {
        pinMode(pin, INPUT);
        output = "gpio: GPIO" + String(pin) + " -> INPUT\n";
      } else if (modeArg == "up") {
        pinMode(pin, INPUT_PULLUP);
        output = "gpio: GPIO" + String(pin) + " -> INPUT_PULLUP\n";
      } else if (modeArg == "down") {
        pinMode(pin, INPUT_PULLDOWN);
        output = "gpio: GPIO" + String(pin) + " -> INPUT_PULLDOWN\n";
      } else {
        output = "gpio: Unknown mode: '" + modeArg + "'\n";
        output += "  Use: low|high (output)  in (input)  up (pullup)  down (pulldown)\n";
      }
    } else if (args.isEmpty()) {
      // 显示所有安全 GPIO 状态（不改变引脚模式）
      output += "GPIO Status:\n";
      output += " PIN | STATE (read only)\n";
      output += "-----+------------------\n";
      for (int pin = 0; pin <= 48; pin++) {
        if (isSafeGpio(pin)) {
          int state = digitalRead(pin);
          output += "  " + String(pin) + (pin < 10 ? "  | " : " | ");
          output += String(state ? "HIGH" : "LOW ") + "\n";
          yield();
        }
      }
    } else {
      // 查看指定 GPIO 状态（不改变引脚模式）
      int pin = args.toInt();
      if (!isSafeGpio(pin)) { output = "gpio: GPIO" + String(pin) + " is not safe\nSafe: " + getSafeGpioList() + "\n"; return; }
      int state = digitalRead(pin);
      output = "gpio: GPIO" + String(pin) + " = " + (state ? "HIGH" : "LOW") + "\n";
    }
    return;
  }

  if (cmd == "adc" || cmd.startsWith("adc ")) {
    String arg = (cmd == "adc") ? "" : cmd.substring(4); arg.trim();
    if (arg.isEmpty()) { output = "adc: Usage: adc <pin>\nValid pins: " + getAdc1ValidPins() + "\n"; return; }
    int adcPin = arg.toInt();
    if (!isAdc1Pin(adcPin)) { output += "adc: GPIO" + String(adcPin) + " is not a valid ADC1 pin\nValid: " + getAdc1ValidPins() + "\n"; g_lastExitCode = 1; }
    else { analogReadResolution(12); int raw = analogRead(adcPin); output += "adc: GPIO" + String(adcPin) + ": " + String(raw) + " / " + String((raw/4095.0)*3.3, 2) + "V\n"; }
    return;
  }

  if (!cmd.isEmpty()) { output = "Command not found: " + cmd + "\nType 'help' for available commands.\n"; g_lastExitCode = 1; }
}

#endif

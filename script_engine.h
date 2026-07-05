#ifndef ESP_SCRIPT_ENGINE_H
#define ESP_SCRIPT_ENGINE_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <LittleFS.h>

// ========== 全局状态 extern ==========
extern std::map<String, String> scriptVars;
extern std::vector<String> scriptInputLines;
extern int    scriptInputIndex;
extern bool   scriptWaitingInput;
extern String scriptInputVarName;
extern int    g_lastExitCode;

// ========== 前置声明 ==========
void executeCommand(String cmd, String& output, String& newPrompt, bool& clearTerminal, String& dlFileName, String& dlContent, bool& triggerUpload);

// ========== 辅助函数 ==========
String extractSubstring(String str, int start, int end) {
  String res = "";
  for (int i = start; i < end && i < str.length(); i++) res += str[i];
  return res;
}

int calculateExpression(String expr) {
  expr.trim();
  if (expr.isEmpty()) return 0;
  int parenDepth = 0, lastOpenParen = -1;
  for (int i = 0; i < expr.length(); i++) {
    if (expr[i] == '(') { lastOpenParen = i; parenDepth++; }
    else if (expr[i] == ')') {
      parenDepth--;
      if (parenDepth == 0 && lastOpenParen != -1) {
        String left = extractSubstring(expr, 0, lastOpenParen);
        String mid = extractSubstring(expr, lastOpenParen + 1, i);
        String right = extractSubstring(expr, i + 1, expr.length());
        return calculateExpression(left + String(calculateExpression(mid)) + right);
      }
    }
  }
  String clean = "";
  for (int i = 0; i < expr.length(); i++)
    if (expr[i] != ' ') clean += expr[i];
  if (clean.isEmpty()) return 0;
  std::vector<int> nums;
  std::vector<char> ops;
  String numStr;
  for (int i = 0; i <= clean.length(); i++) {
    char c = (i == clean.length()) ? '+' : clean[i];
    if (isdigit(c) || (c == '-' && (i == 0 || !isdigit(clean[i - 1])))) numStr += c;
    else {
      if (!numStr.isEmpty()) { nums.push_back(numStr.toInt()); numStr = ""; }
      if (i < clean.length()) ops.push_back(c);
    }
  }
  if (nums.empty()) return 0;
  for (int i = 0; i < ops.size();) {
    if (ops[i] == '*' || ops[i] == '/' || ops[i] == '%') {
      int result = (ops[i] == '*') ? nums[i] * nums[i + 1] : (ops[i] == '/') ? (nums[i + 1] ? nums[i] / nums[i + 1] : 0) : (nums[i + 1] ? nums[i] % nums[i + 1] : 0);
      nums[i] = result; nums.erase(nums.begin() + i + 1); ops.erase(ops.begin() + i);
    } else i++;
  }
  int result = nums[0];
  for (int i = 0; i < ops.size() && i + 1 < nums.size(); i++)
    result = (ops[i] == '+') ? result + nums[i + 1] : result - nums[i + 1];
  return result;
}

String substituteVariables(String line) {
  int pos = 0;
  while ((pos = line.indexOf('$', pos)) != -1) {
    int end = pos + 1;
    if (end >= line.length()) break;
    if (line[end] == '?') end++;
    else while (end < line.length() && (isalnum(line[end]) || line[end] == '_')) end++;
    if (end == pos + 1) { pos++; continue; }
    String varName = line.substring(pos + 1, end); varName.trim();
    String replacement = (varName == "?") ? String(g_lastExitCode) : (scriptVars.count(varName) ? scriptVars[varName] : "");
    line = line.substring(0, pos) + replacement + line.substring(end);
    pos = pos + replacement.length();
    if (pos > line.length()) pos = line.length();
  }
  return line;
}

bool containsAnyChar(String str, const char* chars) {
  for (int i = 0; chars[i] != 0; i++)
    if (str.indexOf(chars[i]) != -1) return true;
  return false;
}

bool evaluateCondition(String condition) {
  condition.trim();
  if (condition.startsWith("(") && condition.endsWith(")")) {
    condition = condition.substring(1, condition.length() - 1); condition.trim();
  }
  const char* ops[] = { "==", "!=", "<=", ">=", "<", ">" };
  int foundOp = -1, opPos = -1;
  for (int i = 0; i < 6; i++) {
    int p = condition.indexOf(ops[i]);
    if (p != -1 && (opPos == -1 || p < opPos)) { opPos = p; foundOp = i; }
  }
  if (foundOp == -1) return !condition.isEmpty();
  String left = condition.substring(0, opPos), right = condition.substring(opPos + strlen(ops[foundOp]));
  left.trim(); right.trim();
  int leftNum = calculateExpression(left), rightNum = calculateExpression(right);
  bool leftIsNum = (!left.isEmpty() && (String(leftNum) == left || containsAnyChar(left, "+-*/%")));
  bool rightIsNum = (!right.isEmpty() && (String(rightNum) == right || containsAnyChar(right, "+-*/%")));
  if (leftIsNum && rightIsNum) {
    switch (foundOp) {
      case 0: return leftNum == rightNum;
      case 1: return leftNum != rightNum;
      case 2: return leftNum <= rightNum;
      case 3: return leftNum >= rightNum;
      case 4: return leftNum < rightNum;
      case 5: return leftNum > rightNum;
    }
  }
  if (foundOp == 0) return left == right;
  if (foundOp == 1) return left != right;
  return false;
}

int findBlockEndEx(const std::vector<String>& lines, int startLine, const char* endKeyword) {
  std::vector<String> stack;
  stack.push_back(endKeyword);
  for (int i = startLine; i < (int)lines.size(); i++) {
    String line = lines[i]; line.trim();
    if (line.startsWith("if ")) stack.push_back("fi");
    else if (line.startsWith("elif ")) { if (stack.empty() || stack.back() != "fi") return -1; }
    else if (line.startsWith("for ") || line.startsWith("while ")) stack.push_back("done");
    else if (line == "else") { if (stack.empty() || stack.back() != "fi") return -1; }
    else if (line == "fi" || line == "done") {
      if (stack.empty()) return -1;
      String expected = stack.back(); stack.pop_back();
      if (line != expected) return -1;
      if (stack.empty()) return i;
    }
  }
  return -1;
}

bool loadScriptInput(const String& inputPath) {
  scriptInputLines.clear(); scriptInputIndex = 0;
  if (!LittleFS.exists(inputPath)) return false;
  File f = LittleFS.open(inputPath, "r");
  if (!f || f.isDirectory()) { f.close(); return false; }
  String line;
  while (f.available()) {
    char c = f.read();
    if (c == '\n') { scriptInputLines.push_back(line); line = ""; }
    else if (c != '\r') line += c;
  }
  if (!line.isEmpty()) scriptInputLines.push_back(line);
  f.close();
  return true;
}

bool scriptReadInput(String& varName, String& output) {
  if (scriptInputIndex < (int)scriptInputLines.size()) {
    scriptVars[varName] = scriptInputLines[scriptInputIndex++];
    output += "  > script: read " + varName + " = '" + scriptVars[varName] + "'\n";
    return true;
  }
  output += "  > script: Warning: No more input data\n";
  scriptVars[varName] = "";
  return false;
}

// ========== 前置声明 ==========
void executeScriptBlockEx(const std::vector<String>& lines, int startLine, int endLine, String& output, bool* outBreak = nullptr, bool* outContinue = nullptr);

bool executeScriptLine(const String& line, const std::vector<String>& lines, int& lineIdx, String& output, bool& shouldBreak, bool& shouldContinue) {
  shouldBreak = shouldContinue = false;
  String trimmed = line; trimmed.trim();
  if (trimmed.isEmpty() || trimmed.startsWith("#")) return true;
  String processed = substituteVariables(trimmed);
  if (processed == "break") { shouldBreak = true; return true; }
  if (processed == "continue") { shouldContinue = true; return true; }
  if (processed.startsWith("read ")) {
    String varName = processed.substring(5); varName.trim();
    if (varName.isEmpty()) { output += "  > script: Error: read requires a variable name\n"; return true; }
    scriptReadInput(varName, output);
    return true;
  }
  if (processed == "sleep" || processed.startsWith("sleep ")) {
    String arg = (processed == "sleep") ? "" : processed.substring(6); arg.trim();
    if (arg.isEmpty()) { output += "  > sleep: Usage: sleep <seconds> or <milliseconds>ms\n"; return true; }
    unsigned long delayMs = arg.endsWith("ms") ? arg.substring(0, arg.length() - 2).toInt() : arg.toInt() * 1000;
    output += "  > sleep: Waiting " + arg + "...\n";
    for (unsigned long elapsed = 0; elapsed < delayMs; elapsed += 50) { delay(min((unsigned long)50, delayMs - elapsed)); yield(); }
    return true;
  }
  if (processed.startsWith("set ")) {
    String assign = processed.substring(4);
    int eqPos = assign.indexOf('=');
    if (eqPos != -1) {
      String var = assign.substring(0, eqPos), val = assign.substring(eqPos + 1);
      var.trim(); val.trim();
      int calcResult = calculateExpression(val);
      bool isExpr = containsAnyChar(val, "+-*/%()");
      scriptVars[var] = (isExpr || val == String(calcResult)) ? String(calcResult) : (val.startsWith("\"") && val.endsWith("\"") ? val.substring(1, val.length() - 1) : val);
    }
    return true;
  }
  if (processed.startsWith("if ")) {
    int fiLine = findBlockEndEx(lines, lineIdx + 1, "fi");
    if (fiLine == -1) { output += "  > script: Error: missing 'fi'\n"; return false; }
    struct Branch { int line; String cond; };
    std::vector<Branch> branches;
    String cond0 = processed.substring(3);
    int thenPos0 = cond0.indexOf(" then");
    if (thenPos0 != -1) cond0 = cond0.substring(0, thenPos0);
    cond0.trim();
    branches.push_back({lineIdx, substituteVariables(cond0)});
    for (int j = lineIdx + 1; j < fiLine; j++) {
      String temp = lines[j]; temp.trim();
      if (temp.startsWith("elif ")) {
        String elifCond = temp.substring(5);
        int tp = elifCond.indexOf(" then");
        if (tp != -1) elifCond = elifCond.substring(0, tp);
        elifCond.trim();
        branches.push_back({j, substituteVariables(elifCond)});
      } else if (temp == "else") branches.push_back({j, "1"});
    }
    for (int b = 0; b < (int)branches.size(); b++) {
      if (evaluateCondition(branches[b].cond)) {
        int blockStart = branches[b].line + 1;
        int blockEnd = (b + 1 < (int)branches.size()) ? branches[b + 1].line : fiLine;
        executeScriptBlockEx(lines, blockStart, blockEnd, output);
        break;
      }
    }
    lineIdx = fiLine;
    return true;
  }
  if (processed.startsWith("while ")) {
    String condition = trimmed.substring(6);
    int doPos = condition.indexOf(" do");
    if (doPos != -1) condition = condition.substring(0, doPos);
    condition.trim();
    int doneLine = findBlockEndEx(lines, lineIdx + 1, "done");
    if (doneLine == -1) { output += "  > script: Error: missing 'done' for while\n"; return false; }
    for (int iter = 0; iter < 500; iter++) {
      if (!evaluateCondition(substituteVariables(condition))) break;
      bool loopBreak = false, loopContinue = false;
      executeScriptBlockEx(lines, lineIdx + 1, doneLine, output, &loopBreak, &loopContinue);
      if (loopBreak) break;
      if (loopContinue) { yield(); continue; }
      yield();
    }
    lineIdx = doneLine;
    return true;
  }
  if (processed.startsWith("for ")) {
    String rest = processed.substring(4);
    int inPos = rest.indexOf(" in "), doPos = rest.indexOf(" do");
    if (inPos == -1 || doPos == -1) { output += "  > script: Syntax error: invalid for loop\n"; return false; }
    String varName = rest.substring(0, inPos), range = rest.substring(inPos + 4, doPos);
    varName.trim(); range.trim();
    int dotPos = range.indexOf("..");
    if (dotPos == -1) { output += "  > script: Syntax error: use 'for i in 1..5 do'\n"; return false; }
    int start = calculateExpression(substituteVariables(range.substring(0, dotPos)));
    int end = calculateExpression(substituteVariables(range.substring(dotPos + 2)));
    int doneLine = findBlockEndEx(lines, lineIdx + 1, "done");
    if (doneLine == -1) { output += "  > script: Error: missing 'done'\n"; return false; }
    if (start <= end) {
      for (int j = start; j <= end; j++) {
        scriptVars[varName] = String(j);
        bool loopBreak = false, loopContinue = false;
        executeScriptBlockEx(lines, lineIdx + 1, doneLine, output, &loopBreak, &loopContinue);
        if (loopBreak) break;
        if (loopContinue) continue;
        yield();
      }
    } else {
      for (int j = start; j >= end; j--) {
        scriptVars[varName] = String(j);
        bool loopBreak = false, loopContinue = false;
        executeScriptBlockEx(lines, lineIdx + 1, doneLine, output, &loopBreak, &loopContinue);
        if (loopBreak) break;
        if (loopContinue) continue;
        yield();
      }
    }
    lineIdx = doneLine;
    return true;
  }
  String cmdOutput, dummyPrompt, dummyDlF, dummyDlC;
  bool dummyClear, dummyUl;
  executeCommand(processed, cmdOutput, dummyPrompt, dummyClear, dummyDlF, dummyDlC, dummyUl);
  if (!cmdOutput.isEmpty()) {
    output += "  > ";
    int start = 0;
    for (size_t i = 0; i < cmdOutput.length(); i++) {
      if (cmdOutput[i] == '\n') { output += cmdOutput.substring(start, i + 1); if (i + 1 < cmdOutput.length()) output += "  > "; start = i + 1; }
    }
    if (start < cmdOutput.length()) output += cmdOutput.substring(start);
  }
  return true;
}

void executeScriptBlockEx(const std::vector<String>& lines, int startLine, int endLine, String& output, bool* outBreak, bool* outContinue) {
  if (startLine < 0 || endLine > (int)lines.size() || startLine >= endLine) { output += "  > script: Error: invalid block range\n"; return; }
  for (int i = startLine; i < endLine;) {
    if (i < 0 || i >= (int)lines.size()) break;
    bool shouldBreak = false, shouldContinue = false;
    if (!executeScriptLine(lines[i], lines, i, output, shouldBreak, shouldContinue)) break;
    if (shouldContinue) { if (outContinue) *outContinue = true; return; }
    if (shouldBreak) { if (outBreak) *outBreak = true; return; }
    i++;
  }
}

#endif

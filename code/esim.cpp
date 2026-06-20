#include "esim.h"
#include "modem.h"
#include "web_handlers.h"

#include <ctype.h>

static const uint8_t ESIM_ISD_R_AID[] = {
  0xA0, 0x00, 0x00, 0x05, 0x59, 0x10, 0x10, 0xFF,
  0xFF, 0xFF, 0xFF, 0x89, 0x00, 0x00, 0x01, 0x00
};

static char s_lastError[128] = "";
static bool s_esimReady = false;

struct TlvNode {
  uint32_t tag;
  const uint8_t* value;
  size_t length;
  size_t nextOffset;
};

static void setError(const char* msg) {
  strncpy(s_lastError, msg, sizeof(s_lastError) - 1);
  s_lastError[sizeof(s_lastError) - 1] = '\0';
}

static void setError(const String& msg) {
  setError(msg.c_str());
}

const char* esimGetLastError() {
  return s_lastError;
}

static bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  return c - 'a' + 10;
}

static bool isHexString(const String& text) {
  if (text.length() == 0 || (text.length() % 2) != 0) return false;
  for (int i = 0; i < text.length(); i++) {
    if (!isHexChar(text.charAt(i))) return false;
  }
  return true;
}

static bool hexToBytes(const String& hex, uint8_t* out, size_t outSize, size_t* outLen) {
  if (!isHexString(hex)) return false;
  size_t len = hex.length() / 2;
  if (len > outSize) return false;
  for (size_t i = 0; i < len; i++) {
    out[i] = (hexNibble(hex.charAt(i * 2)) << 4) | hexNibble(hex.charAt(i * 2 + 1));
  }
  *outLen = len;
  return true;
}

static String bytesToHex(const uint8_t* data, size_t len) {
  static const char digits[] = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

static void copyBytesAsString(char* dst, size_t dstSize, const uint8_t* src, size_t len) {
  if (dstSize == 0) return;
  size_t n = len < (dstSize - 1) ? len : (dstSize - 1);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void bcdToIccid(char* out, size_t outSize, const uint8_t* bcd, size_t bcdLen) {
  if (outSize == 0) return;
  size_t n = 0;
  for (size_t i = 0; i < bcdLen && n + 1 < outSize; i++) {
    uint8_t lo = bcd[i] & 0x0F;
    uint8_t hi = (bcd[i] >> 4) & 0x0F;
    if (lo <= 9) out[n++] = '0' + lo;
    if (hi <= 9 && n + 1 < outSize) out[n++] = '0' + hi;
  }
  out[n] = '\0';
}

static bool iccidToBcd(const String& iccid, uint8_t* out, size_t outSize, size_t* outLen) {
  if (outSize < 10) return false;
  String digits = iccid;
  digits.trim();
  if (digits.length() == 0 || digits.length() > 20) return false;
  for (int i = 0; i < digits.length(); i++) {
    if (!isdigit((unsigned char)digits.charAt(i))) return false;
  }
  memset(out, 0xFF, 10);
  for (int i = 0; i < digits.length(); i += 2) {
    uint8_t lo = digits.charAt(i) - '0';
    uint8_t hi = 0x0F;
    if (i + 1 < digits.length()) hi = digits.charAt(i + 1) - '0';
    out[i / 2] = (hi << 4) | lo;
  }
  *outLen = 10;
  return true;
}

static uint32_t parseInteger(const uint8_t* value, size_t len) {
  uint32_t out = 0;
  for (size_t i = 0; i < len; i++) {
    out = (out << 8) | value[i];
  }
  return out;
}

static bool readTlv(const uint8_t* data, size_t len, size_t offset, TlvNode* node) {
  if (!node || offset >= len) return false;
  size_t pos = offset;
  uint32_t tag = data[pos++];
  if ((tag & 0x1F) == 0x1F) {
    do {
      if (pos >= len) return false;
      tag = (tag << 8) | data[pos];
    } while ((data[pos++] & 0x80) != 0);
  }
  if (pos >= len) return false;
  uint8_t lenByte = data[pos++];
  size_t valueLen = 0;
  if ((lenByte & 0x80) == 0) {
    valueLen = lenByte;
  } else {
    uint8_t lenBytes = lenByte & 0x7F;
    if (lenBytes == 0 || lenBytes > sizeof(size_t) || pos + lenBytes > len) return false;
    for (uint8_t i = 0; i < lenBytes; i++) {
      valueLen = (valueLen << 8) | data[pos++];
    }
  }
  if (pos + valueLen > len) return false;
  node->tag = tag;
  node->value = data + pos;
  node->length = valueLen;
  node->nextOffset = pos + valueLen;
  return true;
}

static bool findChildTag(const uint8_t* data, size_t len, uint32_t tag, TlvNode* found) {
  size_t pos = 0;
  TlvNode node;
  while (readTlv(data, len, pos, &node)) {
    if (node.tag == tag) {
      if (found) *found = node;
      return true;
    }
    pos = node.nextOffset;
  }
  return false;
}

static void appendLength(uint8_t* out, size_t* pos, size_t len) {
  if (len < 0x80) {
    out[(*pos)++] = (uint8_t)len;
  } else if (len <= 0xFF) {
    out[(*pos)++] = 0x81;
    out[(*pos)++] = (uint8_t)len;
  } else {
    out[(*pos)++] = 0x82;
    out[(*pos)++] = (uint8_t)(len >> 8);
    out[(*pos)++] = (uint8_t)len;
  }
}

static void appendTag(uint8_t* out, size_t* pos, uint32_t tag) {
  if (tag > 0xFFFF) out[(*pos)++] = (uint8_t)(tag >> 16);
  if (tag > 0xFF) out[(*pos)++] = (uint8_t)(tag >> 8);
  out[(*pos)++] = (uint8_t)tag;
}

static void appendTlv(uint8_t* out, size_t* pos, uint32_t tag, const uint8_t* value, size_t len) {
  appendTag(out, pos, tag);
  appendLength(out, pos, len);
  if (len > 0) {
    memcpy(out + *pos, value, len);
    *pos += len;
  }
}

static String sendESimATCommand(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);

  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        unsigned long t = millis();
        while (millis() - t < 50) {
          if (Serial1.available()) resp += (char)Serial1.read();
          delay(1);
        }
        return resp;
      }
    }
    delay(1);
  }
  return resp;
}

static String compactAtResponse(const String& resp) {
  String out = resp;
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.trim();
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  if (out.length() > 220) {
    out = out.substring(0, 220) + "...";
  }
  return out;
}

static bool isIgnorableAtLine(const String& line) {
  if (line.length() == 0) return true;
  if (line == "OK" || line == "ERROR") return true;
  if (line.startsWith("AT")) return true;
  if (line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) return true;
  return false;
}

static bool firstDataLine(const String& resp, String* payload) {
  int start = 0;
  while (start <= resp.length()) {
    int end = resp.indexOf('\n', start);
    if (end < 0) end = resp.length();
    String line = resp.substring(start, end);
    line.trim();
    if (!isIgnorableAtLine(line)) {
      *payload = line;
      return true;
    }
    if (end == resp.length()) break;
    start = end + 1;
  }
  return false;
}

static void stripAtTerminator(String* line) {
  line->trim();
  if (line->endsWith("OK")) {
    *line = line->substring(0, line->length() - 2);
    line->trim();
  }
  if (line->endsWith("ERROR")) {
    *line = line->substring(0, line->length() - 5);
    line->trim();
  }
}

static bool parseAtPayload(const String& resp, const char* prefix, String* payload) {
  int idx = resp.indexOf(prefix);
  if (idx >= 0) {
    int start = idx + strlen(prefix);
    int end = resp.indexOf('\n', start);
    if (end < 0) end = resp.length();
    String line = resp.substring(start, end);
    line.trim();
    if (line.length() > 0) {
      stripAtTerminator(&line);
      *payload = line;
      return true;
    }
  }
  if (!firstDataLine(resp, payload)) return false;
  stripAtTerminator(payload);
  return payload->length() > 0;
}

static bool openChannel(String* channel) {
  String aidHex = bytesToHex(ESIM_ISD_R_AID, sizeof(ESIM_ISD_R_AID));
  logCaptureLn(String("eSIM CCHO TX: 打开 ISD-R 通道"));
  String resp = sendESimATCommand((String("AT+CCHO=\"") + aidHex + "\"").c_str(), 10000);
  logCaptureLn(String("eSIM CCHO RX: ") + compactAtResponse(resp));
  String payload;
  if (!parseAtPayload(resp, "+CCHO:", &payload)) {
    if (resp.indexOf("+CME ERROR: 20") >= 0) {
      logCaptureLn(String("eSIM CCHO 返回 CME 20，尝试清理泄漏的逻辑通道后重试"));
      for (int i = 1; i <= 8; i++) {
        String closeCmd = "AT+CCHC=" + String(i);
        String closeResp = sendESimATCommand(closeCmd.c_str(), 2000);
        logCaptureLn(String("eSIM 清理通道 ") + String(i) + ": " + compactAtResponse(closeResp));
      }

      resp = sendESimATCommand((String("AT+CCHO=\"") + aidHex + "\"").c_str(), 10000);
      logCaptureLn(String("eSIM CCHO 重试 RX: ") + compactAtResponse(resp));
      if (!parseAtPayload(resp, "+CCHO:", &payload)) {
        setError(String("打开 eUICC 通道失败，清理通道后仍无法解析响应: ") + compactAtResponse(resp));
        return false;
      }
    } else {
      setError(String("打开 eUICC 通道失败，无法解析响应: ") + compactAtResponse(resp));
      return false;
    }
  }
  payload.trim();
  if (payload.length() >= 2 && payload.charAt(0) == '"' && payload.charAt(payload.length() - 1) == '"') {
    payload = payload.substring(1, payload.length() - 1);
  }
  int channelId = payload.toInt();
  if (payload.length() == 0 || channelId <= 0) {
    setError(String("打开 eUICC 通道返回无效通道号: ") + payload);
    return false;
  }
  *channel = String(channelId);
  logCaptureLn(String("eSIM 通道已打开: ") + *channel);
  return true;
}

static void closeChannel(const String& channel) {
  if (channel.length() == 0) return;
  logCaptureLn(String("eSIM CCHC TX: 关闭通道 ") + channel);
  String resp = sendESimATCommand((String("AT+CCHC=") + channel).c_str(), 5000);
  logCaptureLn(String("eSIM CCHC RX: ") + compactAtResponse(resp));
}

static bool transmitApdu(const String& channel, const uint8_t* tx, size_t txLen, uint8_t** rx, size_t* rxLen) {
  *rx = NULL;
  *rxLen = 0;
  String txHex = bytesToHex(tx, txLen);
  String cmd = "AT+CGLA=" + channel + "," + String(txHex.length()) + ",\"" + txHex + "\"";
  logCaptureLn(String("eSIM CGLA TX: channel=") + channel + ", bytes=" + String(txLen));
  String resp = sendESimATCommand(cmd.c_str(), 30000);
  logCaptureLn(String("eSIM CGLA RX: ") + compactAtResponse(resp));
  String payload;
  if (!parseAtPayload(resp, "+CGLA:", &payload)) {
    setError(String("APDU 传输失败，无法解析响应: ") + compactAtResponse(resp));
    return false;
  }

  int comma = payload.indexOf(',');
  if (comma < 0) {
    // Some modems return only the response APDU HEX without the leading length.
    if (isHexString(payload)) {
      comma = -1;
    } else {
      setError(String("无法解析 CGLA 响应: ") + payload);
      return false;
    }
  }
  String hex = comma >= 0 ? payload.substring(comma + 1) : payload;
  hex.trim();
  if (hex.length() >= 2 && hex.charAt(0) == '"' && hex.charAt(hex.length() - 1) == '"') {
    hex = hex.substring(1, hex.length() - 1);
  }

  size_t maxLen = hex.length() / 2;
  uint8_t* buf = (uint8_t*)malloc(maxLen);
  if (!buf) {
    setError("内存不足，无法接收 APDU 响应");
    return false;
  }
  if (!hexToBytes(hex, buf, maxLen, rxLen)) {
    free(buf);
    setError(String("CGLA 响应不是合法 HEX: ") + hex);
    return false;
  }
  *rx = buf;
  return true;
}

static bool appendResponseData(uint8_t** out, size_t* outLen, const uint8_t* data, size_t len) {
  if (len == 0) return true;
  uint8_t* next = (uint8_t*)realloc(*out, *outLen + len);
  if (!next) {
    setError("内存不足，无法拼接 eUICC 响应");
    free(*out);
    *out = NULL;
    *outLen = 0;
    return false;
  }
  memcpy(next + *outLen, data, len);
  *out = next;
  *outLen += len;
  return true;
}

static uint8_t classByteForChannel(const String& channel) {
  int ch = channel.toInt();
  if (ch <= 0 || ch > 19) return 0x80;
  if (ch < 4) return (0x80 & 0x9C) | ch;
  return (0x80 & 0xB0) | 0x40 | (ch - 4);
}

static bool es10xCommand(const uint8_t* derReq, size_t derReqLen, uint8_t** out, size_t* outLen) {
  *out = NULL;
  *outLen = 0;

  String channel;
  if (!openChannel(&channel)) return false;

  bool ok = false;
  uint8_t cla = classByteForChannel(channel);
  uint8_t apdu[260];
  if (derReqLen > 255) {
    setError("请求过长，当前 Arduino 实现只支持短 APDU");
    goto done;
  }

  apdu[0] = cla;
  apdu[1] = 0xE2;
  apdu[2] = 0x91;
  apdu[3] = 0x00;
  apdu[4] = (uint8_t)derReqLen;
  memcpy(apdu + 5, derReq, derReqLen);

  while (true) {
    uint8_t* rx = NULL;
    size_t rxLen = 0;
    if (!transmitApdu(channel, apdu, 5 + derReqLen, &rx, &rxLen)) {
      free(rx);
      goto done;
    }
    if (rxLen < 2) {
      free(rx);
      setError("APDU 响应长度不足");
      goto done;
    }

    uint8_t sw1 = rx[rxLen - 2];
    uint8_t sw2 = rx[rxLen - 1];
    if (!appendResponseData(out, outLen, rx, rxLen - 2)) {
      free(rx);
      goto done;
    }
    free(rx);

    if (sw1 == 0x61) {
      apdu[0] = cla;
      apdu[1] = 0xC0;
      apdu[2] = 0x00;
      apdu[3] = 0x00;
      apdu[4] = sw2;
      derReqLen = 0;
      continue;
    }
    if ((sw1 & 0xF0) == 0x90) {
      ok = true;
      break;
    }

    char err[64];
    snprintf(err, sizeof(err), "APDU 状态字错误: %02X%02X", sw1, sw2);
    setError(err);
    goto done;
  }

done:
  closeChannel(channel);
  if (!ok) {
    free(*out);
    *out = NULL;
    *outLen = 0;
  }
  return ok;
}

bool esimInit() {
  setError("");
  String resp = sendESimATCommand("AT", 2000);
  if (resp.indexOf("OK") < 0) {
    setError(String("模组 AT 无响应: ") + resp);
    s_esimReady = false;
    return false;
  }

  const char* commands[] = {"AT+CCHO=?", "AT+CCHC=?", "AT+CGLA=?"};
  for (int i = 0; i < 3; i++) {
    logCaptureLn(String("eSIM 能力检测 TX: ") + commands[i]);
    resp = sendESimATCommand(commands[i], 3000);
    logCaptureLn(String("eSIM 能力检测 RX: ") + compactAtResponse(resp));
    if (resp.indexOf("OK") < 0) {
      setError(String("模组不支持 eUICC AT 命令 ") + commands[i] + ": " + resp);
      s_esimReady = false;
      return false;
    }
  }
  s_esimReady = true;
  return true;
}

bool esimGetEID(char* eid, size_t bufferSize) {
  if (!eid || bufferSize == 0) return false;
  eid[0] = '\0';
  uint8_t request[6];
  size_t pos = 0;
  request[pos++] = 0xBF;
  request[pos++] = 0x3E;
  request[pos++] = 0x03;
  request[pos++] = 0x5C;
  request[pos++] = 0x01;
  request[pos++] = 0x5A;

  uint8_t* resp = NULL;
  size_t respLen = 0;
  if (!es10xCommand(request, pos, &resp, &respLen)) return false;

  TlvNode top, eidNode;
  bool ok = readTlv(resp, respLen, 0, &top) &&
            top.tag == 0xBF3E &&
            findChildTag(top.value, top.length, 0x5A, &eidNode);
  if (ok) {
    String eidHex = bytesToHex(eidNode.value, eidNode.length);
    strncpy(eid, eidHex.c_str(), bufferSize - 1);
    eid[bufferSize - 1] = '\0';
  } else {
    setError("无法解析 EID 响应");
  }
  free(resp);
  return ok;
}

int esimGetProfiles(ESimProfile* profiles, int maxProfiles) {
  if (!profiles || maxProfiles <= 0) {
    setError("profile 缓冲区无效");
    return -1;
  }
  memset(profiles, 0, sizeof(ESimProfile) * maxProfiles);

  uint8_t request[] = {
    0xBF, 0x2D, 0x0A,
    0x5C, 0x08,
    0x5A, 0x4F, 0x9F, 0x70, 0x90, 0x91, 0x92, 0x95
  };
  uint8_t* resp = NULL;
  size_t respLen = 0;
  if (!es10xCommand(request, sizeof(request), &resp, &respLen)) return -1;

  TlvNode top, okNode;
  if (!readTlv(resp, respLen, 0, &top) || top.tag != 0xBF2D ||
      !findChildTag(top.value, top.length, 0xA0, &okNode)) {
    free(resp);
    setError("无法解析 profile 列表响应");
    return -1;
  }

  int count = 0;
  size_t pos = 0;
  TlvNode profileNode;
  while (count < maxProfiles && readTlv(okNode.value, okNode.length, pos, &profileNode)) {
    pos = profileNode.nextOffset;
    if (profileNode.tag != 0xE3 && profileNode.tag != 0xBF25) continue;

    ESimProfile& profile = profiles[count];
    profile.state = -1;
    profile.profileClass = -1;

    size_t childPos = 0;
    TlvNode child;
    while (readTlv(profileNode.value, profileNode.length, childPos, &child)) {
      childPos = child.nextOffset;
      switch (child.tag) {
        case 0x5A:
          bcdToIccid(profile.iccid, sizeof(profile.iccid), child.value, child.length);
          break;
        case 0x4F: {
          String aid = bytesToHex(child.value, child.length);
          strncpy(profile.isdpAid, aid.c_str(), sizeof(profile.isdpAid) - 1);
          break;
        }
        case 0x9F70:
          profile.state = (int)parseInteger(child.value, child.length);
          break;
        case 0x90:
          copyBytesAsString(profile.nickname, sizeof(profile.nickname), child.value, child.length);
          break;
        case 0x91:
          copyBytesAsString(profile.serviceProviderName, sizeof(profile.serviceProviderName), child.value, child.length);
          break;
        case 0x92:
          copyBytesAsString(profile.profileName, sizeof(profile.profileName), child.value, child.length);
          break;
        case 0x95:
          profile.profileClass = (int)parseInteger(child.value, child.length);
          break;
      }
    }
    count++;
  }

  free(resp);
  return count;
}

static const char* profileOperationReason(int ret, bool enable) {
  switch (ret) {
    case 1: return "ICCID 或 AID 未找到";
    case 2: return enable ? "profile 不是禁用状态" : "profile 不是启用状态";
    case 3: return "被 profile 策略禁止";
    case 4: return "错误的 profile 重新启用操作";
    case -1: return "内部错误，可能是 ICCID/AID 编码非法";
    default: return "未知错误";
  }
}

static bool buildProfileIdentifier(const char* idText, uint8_t* out, size_t outSize, size_t* outLen) {
  String id = String(idText ? idText : "");
  id.trim();
  if (id.length() == 0) {
    setError("ICCID/AID 不能为空");
    return false;
  }

  uint8_t idBytes[16];
  size_t idLen = 0;
  uint32_t tag = 0x5A;
  if (id.length() == 32 && isHexString(id)) {
    tag = 0x4F;
    if (!hexToBytes(id, idBytes, sizeof(idBytes), &idLen)) {
      setError("AID HEX 编码非法");
      return false;
    }
  } else {
    if (!iccidToBcd(id, idBytes, sizeof(idBytes), &idLen)) {
      setError("ICCID 编码非法");
      return false;
    }
  }

  *outLen = 0;
  appendTlv(out, outLen, tag, idBytes, idLen);
  return *outLen <= outSize;
}

static bool profileOperation(uint32_t outerTag, const char* idText, bool refresh, bool enableForReason) {
  uint8_t idTlv[24];
  size_t idTlvLen = 0;
  if (!buildProfileIdentifier(idText, idTlv, sizeof(idTlv), &idTlvLen)) return false;

  uint8_t request[40];
  size_t pos = 0;
  if (outerTag == 0xBF33) {
    appendTlv(request, &pos, outerTag, idTlv, idTlvLen);
  } else {
    uint8_t value[32];
    size_t valueLen = 0;
    appendTlv(value, &valueLen, 0xA0, idTlv, idTlvLen);
    uint8_t refreshValue = refresh ? 0xFF : 0x00;
    appendTlv(value, &valueLen, 0x81, &refreshValue, 1);
    appendTlv(request, &pos, outerTag, value, valueLen);
  }

  uint8_t* resp = NULL;
  size_t respLen = 0;
  if (!es10xCommand(request, pos, &resp, &respLen)) return false;

  TlvNode top, resultNode;
  bool parsed = readTlv(resp, respLen, 0, &top) &&
                top.tag == outerTag &&
                findChildTag(top.value, top.length, 0x80, &resultNode);
  int ret = parsed ? (int)parseInteger(resultNode.value, resultNode.length) : -1;
  free(resp);

  if (!parsed) {
    setError("无法解析 profile 操作响应");
    return false;
  }
  if (ret != 0) {
    setError(profileOperationReason(ret, enableForReason));
    return false;
  }
  return true;
}

bool esimEnableProfile(const char* iccidOrAid) {
  return profileOperation(0xBF31, iccidOrAid, true, true);
}

bool esimDisableProfile(const char* iccidOrAid) {
  return profileOperation(0xBF32, iccidOrAid, true, false);
}

bool esimDeleteProfile(const char* iccidOrAid) {
  return profileOperation(0xBF33, iccidOrAid, false, false);
}

bool esimSwitchProfile(const char* iccidOrAid) {
  return esimEnableProfile(iccidOrAid);
}

bool esimGetNotificationCount(int* count) {
  if (count) *count = 0;
  setError("通知查询暂未实现");
  return false;
}

static void printProfile(const ESimProfile& profile, int index) {
  Serial.print(index);
  Serial.print(". ICCID: ");
  Serial.print(profile.iccid[0] ? profile.iccid : "-");
  Serial.print(" | 状态: ");
  if (profile.state == 1) Serial.print("已启用");
  else if (profile.state == 0) Serial.print("已禁用");
  else Serial.print("未知");
  Serial.print(" | 名称: ");
  if (profile.nickname[0]) Serial.print(profile.nickname);
  else if (profile.profileName[0]) Serial.print(profile.profileName);
  else Serial.print("-");
  if (profile.serviceProviderName[0]) {
    Serial.print(" | 运营商: ");
    Serial.print(profile.serviceProviderName);
  }
  Serial.println();
}

bool handleESimSerialCommand(const String& command) {
  String line = command;
  line.trim();
  if (!line.startsWith("esim")) return false;

  String args = line.substring(4);
  args.trim();
  if (args.length() == 0 || args == "help") {
    Serial.println("eSIM 命令:");
    Serial.println("  esim list");
    Serial.println("  esim switch <iccid>");
    Serial.println("  esim enable <iccid>");
    Serial.println("  esim disable <iccid>");
    Serial.println("  esim eid");
    return true;
  }

  if (args == "list") {
    ESimProfile profiles[10];
    int count = esimGetProfiles(profiles, 10);
    if (count < 0) {
      Serial.print("获取 profile 列表失败: ");
      Serial.println(esimGetLastError());
      return true;
    }
    Serial.print("profile 数量: ");
    Serial.println(count);
    for (int i = 0; i < count; i++) {
      printProfile(profiles[i], i + 1);
    }
    return true;
  }

  if (args == "eid") {
    char eid[40];
    if (esimGetEID(eid, sizeof(eid))) {
      Serial.print("EID: ");
      Serial.println(eid);
    } else {
      Serial.print("获取 EID 失败: ");
      Serial.println(esimGetLastError());
    }
    return true;
  }

  int space = args.indexOf(' ');
  String action = space >= 0 ? args.substring(0, space) : args;
  String id = space >= 0 ? args.substring(space + 1) : "";
  id.trim();
  if (id.length() == 0) {
    Serial.println("缺少 ICCID/AID 参数");
    return true;
  }

  bool ok = false;
  if (action == "switch" || action == "enable") {
    ok = esimSwitchProfile(id.c_str());
  } else if (action == "disable") {
    ok = esimDisableProfile(id.c_str());
  } else {
    Serial.println("未知 eSIM 命令，输入 esim help 查看帮助");
    return true;
  }

  if (ok) {
    Serial.print("操作成功: ");
    Serial.println(action);
  } else {
    Serial.print("操作失败: ");
    Serial.println(esimGetLastError());
  }
  return true;
}

bool handleSerialConsole() {
  static String line;
  bool consumed = false;

  while (Serial.available()) {
    char c = (char)Serial.read();
    consumed = true;

    if (c == 0x1A) {
      Serial1.write(0x1A);
      line = "";
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (line.length() > 0) {
        String command = line;
        line = "";
        if (!handleESimSerialCommand(command)) {
          Serial1.println(command);
        }
      }
      continue;
    }
    if (line.length() < 160) {
      line += c;
    }
  }

  return consumed;
}

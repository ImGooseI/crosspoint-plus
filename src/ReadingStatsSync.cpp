#include "ReadingStatsSync.h"

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace readingStatsSync {
namespace {
constexpr char OUTBOX_PATH[] = "/.crosspoint/reading_sessions.outbox";
constexpr char OUTBOX_TEMP_PATH[] = "/.crosspoint/reading_sessions.tmp";
constexpr char SUMMARY_PATH[] = "/.crosspoint/reading_stats_remote.dat";
constexpr char SUMMARY_TEMP_PATH[] = "/.crosspoint/reading_stats_remote.tmp";
constexpr char ACTIVITY_PATH[] = "/.crosspoint/reading_activity_remote.dat";
constexpr char ACTIVITY_TEMP_PATH[] = "/.crosspoint/reading_activity_remote.tmp";
constexpr char SESSION_DAYS_PATH[] = "/.crosspoint/reading_session_days_remote.dat";
constexpr char SESSION_DAYS_TEMP_PATH[] = "/.crosspoint/reading_session_days_remote.tmp";
constexpr uint16_t OUTBOX_VERSION = 1;
constexpr size_t MAX_UPLOAD_SESSIONS = 20;
constexpr size_t MAX_OUTBOX_SESSIONS = 256;
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

#pragma pack(push, 1)
struct OutboxHeader {
  char magic[4];
  uint16_t version;
  uint16_t recordSize;
};

struct OutboxRecord {
  char document[33];
  int64_t startedAt;
  int64_t endedAt;
  uint32_t seconds;
  int16_t utcOffsetMinutes;
  uint32_t idSeed;
};

struct SummaryCache {
  char magic[4];
  uint16_t version;
  uint16_t reserved;
  uint32_t accountKey;
  RemoteSummary summary;
};

struct ActivityHeader {
  char magic[4];
  uint16_t version;
  uint16_t recordSize;
  uint32_t accountKey;
};

struct ActivityRecord {
  uint32_t dayKey;
  uint32_t pages;
};

struct SessionDaysHeader {
  char magic[4];
  uint16_t version;
  uint16_t recordSize;
  uint32_t accountKey;
};

struct SessionDayRecord {
  uint32_t dayKey;
  uint32_t seconds;
};
#pragma pack(pop)

constexpr OutboxHeader HEADER{{'C', 'P', 'S', 'R'}, OUTBOX_VERSION, sizeof(OutboxRecord)};
constexpr char SUMMARY_MAGIC[4] = {'C', 'P', 'S', 'U'};

bool validDocument(const char* document) {
  if (!document || strlen(document) != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char c = document[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

uint32_t accountKey() {
  uint32_t hash = 2166136261u;
  const auto add = [&hash](const std::string& value) {
    for (const unsigned char c : value) {
      hash ^= c;
      hash *= 16777619u;
    }
    hash ^= 0xff;
    hash *= 16777619u;
  };
  add(KOREADER_STORE.getBaseUrl());
  add(KOREADER_STORE.getUsername());
  return hash ? hash : 1;
}

std::string deviceId() {
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return {};
  char id[32];
  snprintf(id, sizeof(id), "crosspoint-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return id;
}

void applyHeaders(freeink::SecureHttpClient& http) {
  bool customAuthorization = false;
  for (const auto& header : KOREADER_STORE.getCustomHeaders()) {
    if (header.name.empty()) continue;
    http.addHeader(header.name, header.value);
    if (strcasecmp(header.name.c_str(), "Authorization") == 0) customAuthorization = true;
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  if (!customAuthorization) http.addHeader("Authorization", KOREADER_STORE.getBasicAuthorization());
  http.addHeader("Content-Type", "application/json");
}

bool loadRecords(std::vector<OutboxRecord>& records) {
  records.clear();
  HalFile file;
  if (!Storage.openFileForRead("STAT", OUTBOX_PATH, file)) return true;
  if (file.fileSize() < sizeof(OutboxHeader)) return false;
  OutboxHeader header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || memcmp(header.magic, HEADER.magic, 4) != 0 ||
      header.version != OUTBOX_VERSION || header.recordSize != sizeof(OutboxRecord)) {
    LOG_ERR("STAT", "Reading session outbox header is invalid");
    return false;
  }
  const uint64_t count = (file.fileSize() - sizeof(OutboxHeader)) / sizeof(OutboxRecord);
  if (count > MAX_OUTBOX_SESSIONS) {
    LOG_ERR("STAT", "Reading session outbox exceeds its safety limit");
    return false;
  }
  records.reserve(static_cast<size_t>(count));
  OutboxRecord record{};
  for (uint64_t i = 0; i < count; ++i) {
    if (file.read(&record, sizeof(record)) != sizeof(record)) return false;
    records.push_back(record);
  }
  return true;
}

bool writeRecords(const std::vector<OutboxRecord>& records) {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("STAT", OUTBOX_TEMP_PATH, file)) return false;
  if (file.write(&HEADER, sizeof(HEADER)) != sizeof(HEADER)) return false;
  for (const auto& record : records) {
    if (file.write(&record, sizeof(record)) != sizeof(record)) return false;
  }
  file.flush();
  file.close();
  return Storage.replaceFile(OUTBOX_TEMP_PATH, OUTBOX_PATH);
}

std::string sessionId(const OutboxRecord& record) {
  char id[64];
  snprintf(id, sizeof(id), "r%.32s_%lld_%08lx", record.document, static_cast<long long>(record.startedAt),
           static_cast<unsigned long>(record.idSeed));
  return id;
}

bool enoughHeap() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes >= MIN_FREE_FOR_TLS && heap.largestBlockBytes >= MIN_BLOCK_FOR_TLS) return true;
  LOG_ERR("STAT", "Insufficient heap for stats sync TLS handshake");
  return false;
}

int localUtcOffsetMinutes(const time_t stamp) {
  std::tm local{};
  std::tm utc{};
  if (!localtime_r(&stamp, &local) || !gmtime_r(&stamp, &utc)) return 0;
  // ESP-IDF's tm omits tm_gmtoff. Interpret the UTC fields as local time to
  // recover the signed local offset while preserving the date's DST state.
  utc.tm_isdst = local.tm_isdst;
  const time_t utcFieldsAsLocal = mktime(&utc);
  return static_cast<int>(difftime(stamp, utcFieldsAsLocal) / 60);
}

bool writeRemoteSummary(const RemoteSummary& summary) {
  Storage.mkdir("/.crosspoint");
  SummaryCache cache{};
  memcpy(cache.magic, SUMMARY_MAGIC, sizeof(cache.magic));
  cache.version = 1;
  cache.accountKey = accountKey();
  cache.summary = summary;
  HalFile file;
  if (!Storage.openFileForWrite("STAT", SUMMARY_TEMP_PATH, file)) return false;
  if (file.write(&cache, sizeof(cache)) != sizeof(cache)) return false;
  file.flush();
  file.close();
  return Storage.replaceFile(SUMMARY_TEMP_PATH, SUMMARY_PATH);
}

bool fetchRemoteSummary() {
  if (!KOREADER_STORE.hasCredentials() || KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT ||
      !enoughHeap()) {
    return false;
  }
  const int64_t now = trustedtime::trustedNow();
  if (now <= 0) return false;
  const time_t stamp = static_cast<time_t>(now);
  const int jsTimezoneOffset = -localUtcOffsetMinutes(stamp);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/overview?tz=" +
                          std::to_string(jsTimezoneOffset) + "&days=3650&week_start=1";
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return false;
  applyHeaders(http);
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    http.end();
    LOG_ERR("STAT", "Reading stats overview HTTP %d", status);
    return false;
  }
  const std::string& response = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, response.c_str()) || !doc["seconds"].is<uint64_t>() || !doc["sessions"].is<uint64_t>() ||
      !doc["pages_read"].is<uint64_t>() || !doc["books_finished"].is<uint32_t>() ||
      !doc["reading_days"].is<uint32_t>() || !doc["current_streak"].is<uint32_t>() ||
      !doc["longest_streak"].is<uint32_t>() || !doc["today_seconds"].is<uint64_t>() ||
      !doc["week_seconds"].is<uint64_t>()) {
    LOG_ERR("STAT", "Invalid reading stats overview response");
    return false;
  }
  RemoteSummary summary{};
  summary.seconds = doc["seconds"].as<uint64_t>();
  summary.pages = doc["pages_read"].as<uint64_t>();
  summary.completedBooks = doc["books_finished"].as<uint32_t>();
  summary.readingDays = doc["reading_days"].as<uint32_t>();
  summary.currentStreak = doc["current_streak"].as<uint32_t>();
  summary.longestStreak = doc["longest_streak"].as<uint32_t>();
  summary.todaySeconds = doc["today_seconds"].as<uint64_t>();
  summary.weekSeconds = doc["week_seconds"].as<uint64_t>();
  if (!writeRemoteSummary(summary)) {
    LOG_ERR("STAT", "Could not cache reading stats overview");
    return false;
  }
  return true;
}

int dayOfYear(const int year, const int month, const int day) {
  static constexpr uint16_t starts[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return starts[month] + day - 1 + ((leap && month > 2) ? 1 : 0);
}

class ActivityPagesParser {
 public:
  explicit ActivityPagesParser(HalFile& output) : output(output) {
    filter["day"] = true;
    filter["pages"] = true;
  }

  bool feed(const uint8_t* bytes, const size_t length) {
    for (size_t i = 0; i < length; ++i) {
      if (!feedByte(static_cast<char>(bytes[i]))) return false;
    }
    return true;
  }

  bool complete() const { return sawDays && daysClosed && !capturing; }

 private:
  HalFile& output;
  JsonDocument filter;
  JsonDocument dayDoc;
  std::string dayObject;
  size_t dayCount = 0;
  int rootDepth = 0;
  int dayDepth = 0;
  bool sawDays = false;
  bool daysClosed = false;
  bool capturing = false;
  bool inDaysArray = false;
  bool inString = false;
  bool escaped = false;
  bool keepRootKey = false;
  std::string rootKey;
  uint8_t keyState = 0;

  bool emitDay() {
    dayDoc.clear();
    if (deserializeJson(dayDoc, dayObject.c_str(), DeserializationOption::Filter(filter))) return false;
    const JsonString date = dayDoc["day"].as<JsonString>();
    if (date.isNull() || date.size() != 10 || !dayDoc["pages"].is<uint32_t>()) return false;
    const char* d = date.c_str();
    if (d[4] != '-' || d[7] != '-') return false;
    const int year = (d[0] - '0') * 1000 + (d[1] - '0') * 100 + (d[2] - '0') * 10 + d[3] - '0';
    const int month = (d[5] - '0') * 10 + d[6] - '0';
    const int day = (d[8] - '0') * 10 + d[9] - '0';
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    ActivityRecord record{};
    record.dayKey = static_cast<uint32_t>(year * 10000 + month * 100 + day);
    record.pages = dayDoc["pages"].as<uint32_t>();
    if (output.write(&record, sizeof(record)) != sizeof(record)) return false;
    ++dayCount;
    dayObject.clear();
    return true;
  }

  bool feedByte(const char c) {
    if (capturing) {
      dayObject.push_back(c);
      if (dayObject.size() > 16384) return false;
      if (inString) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') inString = false;
        return true;
      }
      if (c == '"') inString = true;
      else if (c == '{' || c == '[') ++dayDepth;
      else if (c == '}' || c == ']') {
        if (--dayDepth == 0) {
          capturing = false;
          return emitDay();
        }
      }
      return true;
    }

    if (inDaysArray) {
      if (inString) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') inString = false;
        return true;
      }
      if (c == '"') inString = true;
      else if (c == '{') {
        capturing = true;
        dayDepth = 1;
        dayObject.assign(1, c);
      } else if (c == ']') {
        inDaysArray = false;
        daysClosed = true;
      } else if (c != ',' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        return false;
      }
      return true;
    }

    if (inString) {
      if (keepRootKey && rootKey.size() < 16) {
        if (escaped) {
          rootKey.push_back(c);
        } else if (c == '\\') {
          escaped = true;
          return true;
        } else if (c != '"') {
          rootKey.push_back(c);
        }
      } else if (escaped) {
        escaped = false;
        return true;
      } else if (c == '\\') {
        escaped = true;
        return true;
      }
      if (c == '"' && !escaped) {
        inString = false;
        if (keepRootKey && rootKey == "days") keyState = 1;
        keepRootKey = false;
        rootKey.clear();
      }
      if (escaped) escaped = false;
      return true;
    }

    if (keyState == 1) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return true;
      if (c == ':') keyState = 2;
      else keyState = 0;
    } else if (keyState == 2) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return true;
      if (c == '[') {
        inDaysArray = true;
        sawDays = true;
        keyState = 0;
        return true;
      }
      keyState = 0;
    }

    if (c == '"') {
      inString = true;
      keepRootKey = rootDepth == 1;
      rootKey.clear();
      escaped = false;
    } else if (c == '{') {
      ++rootDepth;
    } else if (c == '}') {
      --rootDepth;
      if (rootDepth < 0) return false;
    }
    return true;
  }
};

bool fetchRemoteActivityPages() {
  if (!KOREADER_STORE.hasCredentials() || KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT ||
      !enoughHeap()) return false;
  const int64_t now = trustedtime::trustedNow();
  if (now <= 0) return false;
  const time_t stamp = static_cast<time_t>(now);
  const int jsTimezoneOffset = -localUtcOffsetMinutes(stamp);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/activity?tz=" +
                          std::to_string(jsTimezoneOffset);
  Storage.mkdir("/.crosspoint");
  HalFile output;
  if (!Storage.openFileForWrite("STAT", ACTIVITY_TEMP_PATH, output)) return false;
  ActivityHeader header{{'C', 'P', 'A', 'P'}, 1, sizeof(ActivityRecord), accountKey()};
  if (output.write(&header, sizeof(header)) != sizeof(header)) return false;

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return false;
  applyHeaders(http);
  ActivityPagesParser parser(output);
  const int status = http.sendRequest("GET", nullptr, 0,
                                     [&parser](const uint8_t* data, const size_t size) {
                                       return parser.feed(data, size);
                                     });
  const bool responseComplete = http.responseComplete();
  http.end();
  output.flush();
  output.close();
  if (status < 200 || status >= 300 || !responseComplete || !parser.complete()) {
    Storage.remove(ACTIVITY_TEMP_PATH);
    LOG_ERR("STAT", "Could not refresh daily Pages from CrossPoint Sync (HTTP %d)", status);
    return false;
  }
  return Storage.replaceFile(ACTIVITY_TEMP_PATH, ACTIVITY_PATH);
}

class SessionLogParser {
 public:
  explicit SessionLogParser(HalFile& output) : output(output) { line.reserve(256); }

  bool feed(const uint8_t* bytes, const size_t length) {
    for (size_t i = 0; i < length; ++i) {
      const char c = static_cast<char>(bytes[i]);
      if (c == '\n') {
        if (!emitLine()) return false;
      } else if (line.size() >= 2048) {
        return false;
      } else {
        line.push_back(c);
      }
    }
    return true;
  }

  bool complete() { return emitLine(); }

 private:
  HalFile& output;
  std::string line;
  JsonDocument doc;

  bool emitLine() {
    if (line.empty() || line == "\r") {
      line.clear();
      return true;
    }
    doc.clear();
    if (deserializeJson(doc, line) || !doc["start"].is<int64_t>() || !doc["secs"].is<uint32_t>() ||
        !doc["tz"].is<int16_t>()) return false;
    const int64_t localEpoch = doc["start"].as<int64_t>() + static_cast<int64_t>(doc["tz"].as<int16_t>()) * 60;
    if (localEpoch <= 0) return false;
    const time_t stamp = static_cast<time_t>(localEpoch);
    std::tm date{};
    if (!gmtime_r(&stamp, &date)) return false;
    const int year = date.tm_year + 1900;
    if (year < 1970 || year > 2100) return false;
    SessionDayRecord record{};
    record.dayKey = static_cast<uint32_t>(year * 10000 + (date.tm_mon + 1) * 100 + date.tm_mday);
    record.seconds = doc["secs"].as<uint32_t>();
    if (output.write(&record, sizeof(record)) != sizeof(record)) return false;
    line.clear();
    return true;
  }
};

bool fetchRemoteSessions() {
  if (!KOREADER_STORE.hasCredentials() || KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT ||
      !enoughHeap()) return false;
  const int64_t now = trustedtime::trustedNow();
  if (now <= 0) return false;
  const time_t stamp = static_cast<time_t>(now);
  const int jsTimezoneOffset = -localUtcOffsetMinutes(stamp);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/sessions/export?tz=" +
                          std::to_string(jsTimezoneOffset);
  Storage.mkdir("/.crosspoint");
  HalFile output;
  if (!Storage.openFileForWrite("STAT", SESSION_DAYS_TEMP_PATH, output)) return false;
  SessionDaysHeader header{{'C', 'P', 'S', 'D'}, 1, sizeof(SessionDayRecord), accountKey()};
  if (output.write(&header, sizeof(header)) != sizeof(header)) return false;
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return false;
  applyHeaders(http);
  SessionLogParser parser(output);
  const int status = http.sendRequest("GET", nullptr, 0,
                                      [&parser](const uint8_t* data, const size_t size) {
                                        return parser.feed(data, size);
                                      });
  const bool responseComplete = http.responseComplete();
  http.end();
  const bool parsed = parser.complete();
  output.flush();
  output.close();
  if (status < 200 || status >= 300 || !responseComplete || !parsed) {
    Storage.remove(SESSION_DAYS_TEMP_PATH);
    LOG_ERR("STAT", "Could not refresh reading sessions from CrossPoint Sync (HTTP %d)", status);
    return false;
  }
  return Storage.replaceFile(SESSION_DAYS_TEMP_PATH, SESSION_DAYS_PATH);
}
}  // namespace

void recordSession(const std::string& bookPath, const int64_t startedAt, const int64_t endedAt,
                   const uint32_t seconds) {
  if (startedAt <= 0 || endedAt <= startedAt || endedAt - startedAt > 7 * 24 * 60 * 60 || seconds < 10 ||
      seconds > static_cast<uint64_t>(endedAt - startedAt))
    return;
  const std::string document = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                                   ? KOReaderDocumentId::calculateFromFilename(bookPath)
                                   : KOReaderDocumentId::calculate(bookPath);
  if (!validDocument(document.c_str())) return;

  std::vector<OutboxRecord> records;
  if (!loadRecords(records) || records.size() >= MAX_OUTBOX_SESSIONS) return;
  OutboxRecord record{};
  memcpy(record.document, document.c_str(), document.size());
  record.startedAt = startedAt;
  record.endedAt = endedAt;
  record.seconds = seconds;
  record.idSeed = esp_random();
  const time_t stamp = static_cast<time_t>(startedAt);
  const int offset = localUtcOffsetMinutes(stamp);
  record.utcOffsetMinutes = static_cast<int16_t>(std::clamp(offset, -14 * 60, 14 * 60));
  records.push_back(record);
  if (!writeRecords(records)) LOG_ERR("STAT", "Could not persist measured reading session");
}

bool sync() {
  if (!KOREADER_STORE.hasCredentials() || KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT) return true;
  bool uploadSucceeded = true;
  std::vector<OutboxRecord> records;
  if (!loadRecords(records)) return false;
  if (!records.empty() && !enoughHeap()) return false;

  if (!records.empty()) {
    const std::string id = deviceId();
    if (id.empty()) return false;
    JsonDocument doc;
    doc["device_id"] = id;
    JsonArray items = doc["items"].to<JsonArray>();
    const size_t uploadCount = std::min(records.size(), MAX_UPLOAD_SESSIONS);
    for (size_t i = 0; i < uploadCount; ++i) {
      const OutboxRecord& record = records[i];
      JsonObject item = items.add<JsonObject>();
      item["session_id"] = sessionId(record);
      item["document"] = record.document;
      item["started_at"] = record.startedAt;
      item["ended_at"] = record.endedAt;
      item["seconds"] = record.seconds;
      item["utc_offset_minutes"] = record.utcOffsetMinutes;
    }
    if (doc.overflowed()) return false;
    std::string body;
    serializeJson(doc, body);

    freeink::SecureHttpClient http;
    http.setInsecure();
    const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/sessions";
    if (!http.begin(url)) return false;
    applyHeaders(http);
    const int status = http.sendRequest("PUT", body);
    if (status < 200 || status >= 300) {
      http.end();
      LOG_ERR("STAT", "Stats session sync HTTP %d", status);
      uploadSucceeded = false;
    } else {
      const std::string& response = http.getString();
      http.end();
      JsonDocument ack;
      if (deserializeJson(ack, response.c_str()) || !ack["accepted_session_ids"].is<JsonArrayConst>()) {
        uploadSucceeded = false;
      } else {
        std::vector<std::string> accepted;
        for (JsonVariantConst value : ack["accepted_session_ids"].as<JsonArrayConst>()) {
          if (value.is<const char*>()) accepted.emplace_back(value.as<const char*>());
        }
        std::vector<OutboxRecord> remaining;
        remaining.reserve(records.size());
        size_t removed = 0;
        for (size_t i = 0; i < records.size(); ++i) {
          const std::string sid = sessionId(records[i]);
          const bool wasUploaded = i < uploadCount &&
                                   std::find(accepted.begin(), accepted.end(), sid) != accepted.end();
          if (wasUploaded) ++removed;
          else remaining.push_back(records[i]);
        }
        if (removed == 0) {
          uploadSucceeded = false;
        } else if (!writeRecords(remaining)) {
          LOG_ERR("STAT", "Server acknowledged sessions but outbox cleanup failed; safe retry");
          uploadSucceeded = false;
        } else {
          LOG_INF("STAT", "Synced %u measured reading sessions", static_cast<unsigned>(removed));
        }
      }
    }
  }
  const bool summaryUpdated = fetchRemoteSummary();
  const bool activityUpdated = fetchRemoteActivityPages();
  const bool sessionsUpdated = fetchRemoteSessions();
  return uploadSucceeded && summaryUpdated && activityUpdated && sessionsUpdated;
}

bool readRemoteSummary(RemoteSummary& summary) {
  HalFile file;
  if (!Storage.openFileForRead("STAT", SUMMARY_PATH, file) || file.fileSize() != sizeof(SummaryCache)) return false;
  SummaryCache cache{};
  if (file.read(&cache, sizeof(cache)) != sizeof(cache) || memcmp(cache.magic, SUMMARY_MAGIC, 4) != 0 ||
      cache.version != 1 || cache.accountKey != accountKey()) {
    return false;
  }
  summary = cache.summary;
  return true;
}

bool readRemoteDayPages(const int year, uint32_t* pages, const size_t count) {
  if (!pages || count < 366 || year < 1970 || year > 2100) return false;
  HalFile file;
  if (!Storage.openFileForRead("STAT", ACTIVITY_PATH, file) || file.fileSize() < sizeof(ActivityHeader)) return false;
  ActivityHeader header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || memcmp(header.magic, "CPAP", 4) != 0 ||
      header.version != 1 || header.recordSize != sizeof(ActivityRecord) || header.accountKey != accountKey() ||
      (file.fileSize() - sizeof(ActivityHeader)) % sizeof(ActivityRecord) != 0) {
    return false;
  }
  std::fill(pages, pages + count, 0);
  const uint64_t recordCount = (file.fileSize() - sizeof(ActivityHeader)) / sizeof(ActivityRecord);
  ActivityRecord record{};
  for (uint64_t i = 0; i < recordCount; ++i) {
    if (file.read(&record, sizeof(record)) != sizeof(record)) return false;
    const int recordYear = record.dayKey / 10000;
    const int month = (record.dayKey / 100) % 100;
    const int day = record.dayKey % 100;
    if (recordYear != year || month < 1 || month > 12 || day < 1 || day > 31) continue;
    const int index = dayOfYear(year, month, day);
    if (index >= 0 && static_cast<size_t>(index) < count) pages[index] = record.pages;
  }
  return true;
}

bool readRemoteDayStats(const int year, uint32_t* seconds, uint16_t* sessions, const size_t count) {
  if (!seconds || !sessions || count < 366 || year < 1970 || year > 2100) return false;
  HalFile file;
  if (!Storage.openFileForRead("STAT", SESSION_DAYS_PATH, file) || file.fileSize() < sizeof(SessionDaysHeader))
    return false;
  SessionDaysHeader header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || memcmp(header.magic, "CPSD", 4) != 0 ||
      header.version != 1 || header.recordSize != sizeof(SessionDayRecord) || header.accountKey != accountKey() ||
      (file.fileSize() - sizeof(SessionDaysHeader)) % sizeof(SessionDayRecord) != 0) return false;
  std::fill(seconds, seconds + count, 0);
  std::fill(sessions, sessions + count, 0);
  const uint64_t recordCount = (file.fileSize() - sizeof(SessionDaysHeader)) / sizeof(SessionDayRecord);
  SessionDayRecord record{};
  for (uint64_t i = 0; i < recordCount; ++i) {
    if (file.read(&record, sizeof(record)) != sizeof(record)) return false;
    const int recordYear = record.dayKey / 10000;
    const int month = (record.dayKey / 100) % 100;
    const int day = record.dayKey % 100;
    if (recordYear != year || month < 1 || month > 12 || day < 1 || day > 31) continue;
    const int index = dayOfYear(year, month, day);
    if (index < 0 || static_cast<size_t>(index) >= count) continue;
    seconds[index] = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
                                     static_cast<uint64_t>(seconds[index]) + record.seconds));
    if (sessions[index] < UINT16_MAX) ++sessions[index];
  }
  return true;
}

}  // namespace readingStatsSync

#endif

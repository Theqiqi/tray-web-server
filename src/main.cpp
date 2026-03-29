#include "json.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using tray_panel::json::Array;
using tray_panel::json::Object;
using tray_panel::json::Value;

namespace {

std::mutex g_mutex;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string Trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string JsonEscape(const std::string& input) {
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string SerializeJson(const Value& value);

std::string SerializeObject(const Object& object) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& [key, val] : object) {
        if (!first) out << ",";
        first = false;
        out << "\"" << JsonEscape(key) << "\":" << SerializeJson(val);
    }
    out << "}";
    return out.str();
}

std::string SerializeArray(const Array& array) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& item : array) {
        if (!first) out << ",";
        first = false;
        out << SerializeJson(item);
    }
    out << "]";
    return out.str();
}

std::string SerializeJson(const Value& value) {
    if (value.IsNull()) return "null";
    if (value.IsBool()) return value.AsBool() ? "true" : "false";
    if (value.IsNumber()) {
        std::ostringstream out;
        out << std::setprecision(15) << value.AsNumber();
        return out.str();
    }
    if (value.IsString()) return "\"" + JsonEscape(value.AsString()) + "\"";
    if (value.IsArray()) return SerializeArray(value.AsArray());
    return SerializeObject(value.AsObject());
}

std::string SerializePretty(const Value& value, int indent = 0);

std::string SerializePrettyObject(const Object& object, int indent) {
    std::ostringstream out;
    out << "{";
    if (!object.empty()) {
        out << "\n";
        bool first = true;
        for (const auto& [key, val] : object) {
            if (!first) out << ",\n";
            first = false;
            out << std::string(indent + 2, ' ') << "\"" << JsonEscape(key) << "\": " << SerializePretty(val, indent + 2);
        }
        out << "\n" << std::string(indent, ' ');
    }
    out << "}";
    return out.str();
}

std::string SerializePrettyArray(const Array& array, int indent) {
    std::ostringstream out;
    out << "[";
    if (!array.empty()) {
        out << "\n";
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (i > 0) out << ",\n";
            out << std::string(indent + 2, ' ') << SerializePretty(array[i], indent + 2);
        }
        out << "\n" << std::string(indent, ' ');
    }
    out << "]";
    return out.str();
}

std::string SerializePretty(const Value& value, int indent) {
    if (value.IsObject()) return SerializePrettyObject(value.AsObject(), indent);
    if (value.IsArray()) return SerializePrettyArray(value.AsArray(), indent);
    return SerializeJson(value);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read file: " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to write file: " + path.string());
    output << content;
}

Value ParseJsonFile(const fs::path& path) {
    return tray_panel::json::Parse(ReadFile(path));
}

std::optional<std::string> GetOptionalString(const Object& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end() || it->second.IsNull()) return std::nullopt;
    if (it->second.IsString()) return it->second.AsString();
    if (it->second.IsNumber()) {
        std::ostringstream out;
        out << std::setprecision(15) << it->second.AsNumber();
        return out.str();
    }
    return std::nullopt;
}

double GetOptionalNumber(const Object& object, const std::string& key, double fallback = 0.0) {
    const auto it = object.find(key);
    if (it == object.end() || it->second.IsNull()) return fallback;
    if (it->second.IsNumber()) return it->second.AsNumber();
    if (it->second.IsString()) {
        try { return std::stod(it->second.AsString()); } catch (...) { return fallback; }
    }
    return fallback;
}

bool GetOptionalBool(const Object& object, const std::string& key, bool fallback = false) {
    const auto it = object.find(key);
    if (it == object.end() || it->second.IsNull()) return fallback;
    if (it->second.IsBool()) return it->second.AsBool();
    if (it->second.IsString()) return ToLower(it->second.AsString()) == "true";
    return fallback;
}

Object GetObject(const Object& object, const std::string& key) {
    const auto it = object.find(key);
    if (it != object.end() && it->second.IsObject()) return it->second.AsObject();
    return Object{};
}

Array GetArray(const Object& object, const std::string& key) {
    const auto it = object.find(key);
    if (it != object.end() && it->second.IsArray()) return it->second.AsArray();
    return Array{};
}

Object DefaultSettings() {
    return {
        {"host", Value(std::string("0.0.0.0"))},
        {"port", Value(8090.0)},
        {"language", Value(std::string("zh-CN"))},
    };
}

std::string GenerateToken(std::size_t length = 32) {
    static const char alphabet[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string token;
    token.reserve(length);
    for (std::size_t i = 0; i < length; ++i) token.push_back(alphabet[dist(gen)]);
    return token;
}

std::string MakeSlug(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    bool last_dash = false;
    for (unsigned char ch : input) {
        if (std::isalnum(ch)) {
            result.push_back(static_cast<char>(std::tolower(ch)));
            last_dash = false;
            continue;
        }
        if (ch >= 0x80) {
            result.push_back(static_cast<char>(ch));
            last_dash = false;
            continue;
        }
        if (!last_dash && !result.empty()) {
            result.push_back('-');
            last_dash = true;
        }
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result.empty() ? "user" : result;
}

bool IsLegacyUserSlug(const std::string& slug) {
    if (slug == "user") return true;
    if (!StartsWith(slug, "user-")) return false;
    if (slug.size() <= 5) return false;
    return std::all_of(slug.begin() + 5, slug.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string EnsureUniqueSlug(const Array& users, const std::string& slug, const std::optional<std::string>& current_id = std::nullopt) {
    const std::string base = MakeSlug(slug);
    std::string candidate = base;
    int index = 2;
    while (true) {
        bool exists = false;
        for (const auto& item : users) {
            if (!item.IsObject()) continue;
            const auto& object = item.AsObject();
            if (GetOptionalString(object, "slug").value_or("") != candidate) continue;
            if (current_id.has_value() && GetOptionalString(object, "id").value_or("") == *current_id) continue;
            exists = true;
            break;
        }
        if (!exists) return candidate;
        candidate = base + "-" + std::to_string(index++);
    }
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

std::string UrlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const auto decoded = std::strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0') {
                result.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

bool NormalizeUserSlugs(Object& store) {
    bool changed = false;
    auto users = GetArray(store, "users");
    for (auto& item : users) {
        if (!item.IsObject()) continue;
        auto& user = const_cast<Object&>(item.AsObject());
        const auto name = Trim(GetOptionalString(user, "name").value_or(""));
        if (name.empty()) continue;
        const auto current_slug = GetOptionalString(user, "slug").value_or("");
        if (!current_slug.empty() && !IsLegacyUserSlug(current_slug)) continue;
        const auto user_id = GetOptionalString(user, "id");
        const auto next_slug = EnsureUniqueSlug(users, name, user_id);
        if (current_slug != next_slug) {
            user["slug"] = Value(next_slug);
            changed = true;
        }
    }
    if (changed) store["users"] = Value(users);
    return changed;
}

Array NormalizeInboundTags(const Object& object) {
    Array tags;
    const auto it = object.find("inboundTags");
    if (it == object.end() || !it->second.IsArray()) return tags;
    for (const auto& item : it->second.AsArray()) {
        if (item.IsString()) tags.push_back(Value(item.AsString()));
    }
    return tags;
}

std::vector<std::string> ExtractInboundTags(const Object& object) {
    std::vector<std::string> tags;
    const auto it = object.find("inboundTags");
    if (it == object.end() || !it->second.IsArray()) return tags;
    for (const auto& item : it->second.AsArray()) {
        if (item.IsString()) tags.push_back(item.AsString());
    }
    return tags;
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> cookies;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct KernelHttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

class PanelServer {
public:
    PanelServer(fs::path app_root, std::optional<std::string> cli_host, std::optional<int> cli_port);
    int Run();

private:
    fs::path app_root_;
    fs::path public_root_;
    fs::path runtime_root_;
    fs::path adapter_path_;
    fs::path panel_auth_path_;
    fs::path panel_settings_path_;
    fs::path export_users_path_;
    fs::path kernel_pid_path_;
    fs::path kernel_stdout_path_;
    fs::path kernel_stderr_path_;
    std::optional<std::string> cli_host_;
    std::optional<int> cli_port_;

    static int CreateListener(const std::string& host, int port);
    static std::string MimeType(const fs::path& path);
    static void SendAll(int fd, const std::string& data);
    static HttpRequest ReadRequest(int fd);
    static void WriteResponse(int fd, const HttpResponse& response);

    void HandleClient(int client_fd);
    HttpResponse Route(const HttpRequest& request);
    HttpResponse ServeStatic(const std::string& clean_path);
    HttpResponse HandleGet(const HttpRequest& request);
    HttpResponse HandlePost(const HttpRequest& request);
    HttpResponse HandlePut(const HttpRequest& request);
    HttpResponse HandleDelete(const HttpRequest& request);
    HttpResponse HandleSubscription(const HttpRequest& request);
    HttpResponse NotFound() const;
    HttpResponse JsonResponse(int status, std::string reason, const Value& body) const;

    Object LoadAdapter() const;
    Object LoadPanelAuth() const;
    void SavePanelAuth(const Object& auth) const;
    Object LoadSettings() const;
    void SaveSettings(const Object& settings) const;
    Object LoadExportUsers() const;
    void SaveExportUsers(const Object& store) const;
    Object NormalizePanelSettings(const Object& source) const;

    std::optional<Object> GetAuthenticatedSession(const HttpRequest& request, const Object& auth) const;
    void RequireAuth(const HttpRequest& request) const;
    static std::string GetSessionId(const HttpRequest& request);

    std::vector<std::string> GetLocalIps() const;
    std::string ResolvePublicHost(const HttpRequest& request, int fallback_port) const;
    std::string ResolvePanelRedirectHost(const HttpRequest& request, const std::string& configured_host) const;
    static std::string StripPort(const std::string& host_value);

    static std::optional<int> ReadKernelPid(const fs::path& path);
    static void WriteKernelPid(const fs::path& path, pid_t pid);
    static void ClearKernelPid(const fs::path& path);
    static bool IsProcessAlive(pid_t pid);
    std::string ReadRecentLog(const fs::path& path, std::size_t max_bytes = 4096) const;
    fs::path ResolveAppPath(const std::string& path_text) const;
    std::vector<std::string> ResolveStartCommand(const Object& adapter) const;
    void SyncManagementConfig(const Object& adapter) const;
    static void RunPkill(const std::string& binary_name);
    void StopProxyCore(const Object& adapter) const;
    Object GetProxyCoreStatus(const Object& adapter) const;
    void EnsureProxyCoreRunning() const;
    Object StartProxyCore(const Object& adapter) const;
    static void SchedulePanelRestart(int exit_code, std::chrono::seconds delay);

    static std::pair<std::string, int> ParseBaseUrl(const std::string& base_url);
    Value KernelRequest(const Object& adapter, const std::string& path, const std::string& method = "GET",
                        const std::optional<std::string>& body = std::nullopt) const;
    static int Connect(const std::string& host, int port);
    static KernelHttpResponse ReadHttpResponse(int fd);
    static std::optional<std::string> GetQueryParam(const std::string& query, const std::string& key);
    static std::string Join(const std::vector<std::string>& items, const std::string& delimiter);
    static std::string Base64Encode(const std::string& input);
    static Object ParseObjectBody(const std::string& body);
};

PanelServer::PanelServer(fs::path app_root, std::optional<std::string> cli_host, std::optional<int> cli_port)
    : app_root_(std::move(app_root)),
      public_root_(app_root_ / "public"),
      runtime_root_(app_root_ / "runtime"),
      adapter_path_(runtime_root_ / "adapter.json"),
      panel_auth_path_(runtime_root_ / "panel-auth.json"),
      panel_settings_path_(runtime_root_ / "panel-settings.json"),
      export_users_path_(runtime_root_ / "export-users.json"),
      kernel_pid_path_(runtime_root_ / "proxy.pid"),
      kernel_stdout_path_(runtime_root_ / "proxy.stdout.log"),
      kernel_stderr_path_(runtime_root_ / "proxy.stderr.log"),
      cli_host_(std::move(cli_host)),
      cli_port_(cli_port) {}

int PanelServer::CreateListener(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        throw std::runtime_error("getaddrinfo failed");
    }

    int server_fd = -1;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        server_fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (server_fd < 0) continue;
        int reuse = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(server_fd, item->ai_addr, item->ai_addrlen) == 0 && ::listen(server_fd, SOMAXCONN) == 0) break;
        ::close(server_fd);
        server_fd = -1;
    }
    ::freeaddrinfo(result);
    if (server_fd < 0) throw std::runtime_error("failed to bind listener on " + host + ":" + std::to_string(port));
    return server_fd;
}

std::string PanelServer::MimeType(const fs::path& path) {
    const auto ext = ToLower(path.extension().string());
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    return "application/octet-stream";
}

void PanelServer::SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (rc <= 0) throw std::runtime_error("failed to send response");
        sent += static_cast<std::size_t>(rc);
    }
}

HttpRequest PanelServer::ReadRequest(int fd) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t read_size = ::recv(fd, buffer, sizeof(buffer), 0);
        if (read_size <= 0) throw std::runtime_error("failed to read request");
        data.append(buffer, static_cast<std::size_t>(read_size));
        if (data.size() > 1024 * 1024) throw std::runtime_error("request too large");
    }

    const std::string header_text = data.substr(0, header_end);
    std::istringstream stream(header_text);
    std::string request_line;
    std::getline(stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    HttpRequest request;
    std::istringstream request_line_stream(request_line);
    request_line_stream >> request.method >> request.target;
    if (request.method.empty() || request.target.empty()) throw std::runtime_error("invalid request line");

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        request.headers[ToLower(Trim(line.substr(0, pos)))] = Trim(line.substr(pos + 1));
    }

    const auto cookie_it = request.headers.find("cookie");
    if (cookie_it != request.headers.end()) {
        std::istringstream cookie_stream(cookie_it->second);
        std::string item;
        while (std::getline(cookie_stream, item, ';')) {
            const auto pos = item.find('=');
            if (pos == std::string::npos) continue;
            request.cookies[Trim(item.substr(0, pos))] = Trim(item.substr(pos + 1));
        }
    }

    const auto target_pos = request.target.find('?');
    request.path = target_pos == std::string::npos ? request.target : request.target.substr(0, target_pos);
    request.query = target_pos == std::string::npos ? "" : request.target.substr(target_pos + 1);

    std::size_t content_length = 0;
    const auto cl_it = request.headers.find("content-length");
    if (cl_it != request.headers.end()) content_length = static_cast<std::size_t>(std::stoul(cl_it->second));

    request.body = data.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const ssize_t read_size = ::recv(fd, buffer, sizeof(buffer), 0);
        if (read_size <= 0) throw std::runtime_error("incomplete request body");
        request.body.append(buffer, static_cast<std::size_t>(read_size));
    }
    if (request.body.size() > content_length) request.body.resize(content_length);
    return request;
}

void PanelServer::WriteResponse(int fd, const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << response.reason << "\r\n";
    bool has_content_type = false;
    for (const auto& [key, value] : response.headers) {
        if (ToLower(key) == "content-type") has_content_type = true;
        out << key << ": " << value << "\r\n";
    }
    if (!has_content_type) out << "Content-Type: text/plain; charset=utf-8\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    SendAll(fd, out.str());
    SendAll(fd, response.body);
}

HttpResponse PanelServer::JsonResponse(int status, std::string reason, const Value& body) const {
    HttpResponse response;
    response.status = status;
    response.reason = std::move(reason);
    response.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
    response.body = SerializeJson(body);
    return response;
}

Object PanelServer::LoadAdapter() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return ParseJsonFile(adapter_path_).AsObject();
}

Object PanelServer::LoadPanelAuth() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fs::exists(panel_auth_path_)) return ParseJsonFile(panel_auth_path_).AsObject();
    return Object{
        {"username", Value(std::string("admin"))},
        {"password", Value(std::string("admin"))},
        {"sessions", Value(Object{})},
    };
}

void PanelServer::SavePanelAuth(const Object& auth) const {
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteFile(panel_auth_path_, SerializePretty(Value(auth)) + "\n");
}

Object PanelServer::NormalizePanelSettings(const Object& source) const {
    Object merged = DefaultSettings();
    for (const auto& [key, value] : source) merged[key] = value;
    merged["host"] = Value(GetOptionalString(merged, "host").value_or("0.0.0.0"));
    merged["port"] = Value(static_cast<double>(static_cast<int>(GetOptionalNumber(merged, "port", 8090))));
    return merged;
}

Object PanelServer::LoadSettings() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fs::exists(panel_settings_path_)) return NormalizePanelSettings(ParseJsonFile(panel_settings_path_).AsObject());
    return DefaultSettings();
}

void PanelServer::SaveSettings(const Object& settings) const {
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteFile(panel_settings_path_, SerializePretty(Value(NormalizePanelSettings(settings))) + "\n");
}

Object PanelServer::LoadExportUsers() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fs::exists(export_users_path_)) {
        auto root = ParseJsonFile(export_users_path_).AsObject();
        if (root.find("users") == root.end() || !root.at("users").IsArray()) root["users"] = Value(Array{});
        if (NormalizeUserSlugs(root)) WriteFile(export_users_path_, SerializePretty(Value(root)) + "\n");
        return root;
    }
    return Object{{"users", Value(Array{})}};
}

void PanelServer::SaveExportUsers(const Object& store) const {
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteFile(export_users_path_, SerializePretty(Value(store)) + "\n");
}

std::optional<Object> PanelServer::GetAuthenticatedSession(const HttpRequest& request, const Object& auth) const {
    const std::string sid = GetSessionId(request);
    if (sid.empty()) return std::nullopt;
    const auto sessions = GetObject(auth, "sessions");
    const auto it = sessions.find(sid);
    if (it == sessions.end() || !it->second.IsObject()) return std::nullopt;
    return it->second.AsObject();
}

void PanelServer::RequireAuth(const HttpRequest& request) const {
    const auto auth = LoadPanelAuth();
    if (!GetAuthenticatedSession(request, auth).has_value()) {
        throw std::runtime_error("请先登录");
    }
}

std::string PanelServer::GetSessionId(const HttpRequest& request) {
    const auto cookie_it = request.cookies.find("session_id");
    if (cookie_it != request.cookies.end()) return cookie_it->second;
    const auto auth_it = request.headers.find("authorization");
    if (auth_it != request.headers.end() && StartsWith(auth_it->second, "Bearer ")) return auth_it->second.substr(7);
    const auto sid_it = request.headers.find("x-session-id");
    return sid_it == request.headers.end() ? "" : sid_it->second;
}

std::vector<std::string> PanelServer::GetLocalIps() const {
    std::vector<std::string> ips;
    char hostname[256] = {};
    if (::gethostname(hostname, sizeof(hostname)) != 0) return ips;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(hostname, nullptr, &hints, &result) != 0) return ips;

    std::set<std::string> seen;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        char address[INET_ADDRSTRLEN] = {};
        auto* addr = reinterpret_cast<sockaddr_in*>(item->ai_addr);
        if (::inet_ntop(AF_INET, &addr->sin_addr, address, sizeof(address)) == nullptr) continue;
        std::string ip = address;
        if (!StartsWith(ip, "127.") && seen.insert(ip).second) ips.push_back(ip);
    }
    ::freeaddrinfo(result);
    return ips;
}

std::string PanelServer::StripPort(const std::string& host_value) {
    if (!host_value.empty() && host_value.front() == '[') {
        const auto end = host_value.find(']');
        return end == std::string::npos ? host_value : host_value.substr(1, end - 1);
    }
    const auto pos = host_value.find(':');
    return pos == std::string::npos ? host_value : host_value.substr(0, pos);
}

std::string PanelServer::ResolvePublicHost(const HttpRequest& request, int fallback_port) const {
    std::string forwarded;
    const auto xf = request.headers.find("x-forwarded-host");
    if (xf != request.headers.end()) forwarded = xf->second;
    if (forwarded.empty()) {
        const auto host = request.headers.find("host");
        if (host != request.headers.end()) forwarded = host->second;
    }
    if (!forwarded.empty()) {
        const auto pure = StripPort(forwarded);
        if (pure != "127.0.0.1" && pure != "localhost" && pure != "::1" && pure != "0.0.0.0") return forwarded;
    }
    const auto ips = GetLocalIps();
    return (ips.empty() ? std::string("127.0.0.1") : ips.front()) + ":" + std::to_string(fallback_port);
}

std::string PanelServer::ResolvePanelRedirectHost(const HttpRequest& request, const std::string& configured_host) const {
    if (configured_host != "0.0.0.0" && configured_host != "::" && configured_host != "" &&
        configured_host != "127.0.0.1" && configured_host != "localhost" && configured_host != "::1") {
        return configured_host;
    }
    const auto xf = request.headers.find("x-forwarded-host");
    if (xf != request.headers.end() && !xf->second.empty()) return StripPort(xf->second);
    const auto host = request.headers.find("host");
    if (host != request.headers.end() && !host->second.empty()) return StripPort(host->second);
    const auto ips = GetLocalIps();
    return ips.empty() ? std::string("127.0.0.1") : ips.front();
}

std::optional<int> PanelServer::ReadKernelPid(const fs::path& path) {
    try { return std::stoi(Trim(ReadFile(path))); } catch (...) { return std::nullopt; }
}

void PanelServer::WriteKernelPid(const fs::path& path, pid_t pid) {
    WriteFile(path, std::to_string(pid));
}

void PanelServer::ClearKernelPid(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

bool PanelServer::IsProcessAlive(pid_t pid) {
    return pid > 0 && ::kill(pid, 0) == 0;
}

std::string PanelServer::ReadRecentLog(const fs::path& path, std::size_t max_bytes) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) return "";
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string data = buffer.str();
    if (data.size() <= max_bytes) return Trim(data);
    return Trim(data.substr(data.size() - max_bytes));
}

fs::path PanelServer::ResolveAppPath(const std::string& path_text) const {
    fs::path path(path_text);
    return path.is_absolute() ? path : app_root_ / path;
}

std::vector<std::string> PanelServer::ResolveStartCommand(const Object& adapter) const {
    std::vector<std::string> cmd;
    const auto it = adapter.find("startCommand");
    if (it == adapter.end() || !it->second.IsArray()) throw std::runtime_error("missing startCommand");
    for (const auto& item : it->second.AsArray()) {
        if (!item.IsString()) continue;
        auto token = item.AsString();
        if (StartsWith(token, "./")) cmd.push_back((app_root_ / token.substr(2)).string());
        else cmd.push_back(token);
    }
    return cmd;
}

void PanelServer::RunPkill(const std::string& binary_name) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp("pkill", "pkill", "-f", binary_name.c_str(), nullptr);
        _exit(1);
    }
    if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
}

Object PanelServer::ParseObjectBody(const std::string& body) {
    if (Trim(body).empty()) return Object{};
    return tray_panel::json::Parse(body).AsObject();
}

void PanelServer::SyncManagementConfig(const Object& adapter) const {
    const auto [host, port] = ParseBaseUrl(GetOptionalString(adapter, "managementApiBase").value_or("http://127.0.0.1:9090"));
    (void)host;
    const std::string token = GetOptionalString(adapter, "managementApiToken").value_or("");
    std::vector<fs::path> targets;
    targets.push_back(ResolveAppPath(GetOptionalString(adapter, "managedConfigPath").value_or("runtime/managed-config.json")));
    try {
        const auto managed = ParseJsonFile(targets.front()).AsObject();
        const auto management = GetObject(managed, "management");
        if (const auto storage = GetOptionalString(management, "storagePath"); storage.has_value()) {
            targets.push_back(ResolveAppPath(*storage));
        }
    } catch (...) {
    }

    std::set<fs::path> unique(targets.begin(), targets.end());
    for (const auto& path : unique) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (!fs::exists(path)) continue;
        try {
            auto root = ParseJsonFile(path).AsObject();
            auto management = GetObject(root, "management");
            bool changed = false;
            if (static_cast<int>(GetOptionalNumber(management, "port", port)) != port) {
                management["port"] = Value(static_cast<double>(port));
                changed = true;
            }
            if (GetOptionalString(management, "token").value_or("") != token) {
                management["token"] = Value(token);
                changed = true;
            }
            root["management"] = Value(management);
            if (changed) WriteFile(path, SerializePretty(Value(root)) + "\n");
        } catch (...) {
        }
    }
}

void PanelServer::StopProxyCore(const Object& adapter) const {
    const auto pid = ReadKernelPid(kernel_pid_path_).value_or(-1);
    if (IsProcessAlive(pid)) {
        ::kill(pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!IsProcessAlive(pid)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (IsProcessAlive(pid)) ::kill(pid, SIGKILL);
    }
    ClearKernelPid(kernel_pid_path_);

    const auto cmd = ResolveStartCommand(adapter);
    if (!cmd.empty()) {
        RunPkill(fs::path(cmd.front()).filename().string());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

Object PanelServer::GetProxyCoreStatus(const Object& adapter) const {
    const auto pid = ReadKernelPid(kernel_pid_path_).value_or(-1);
    const bool pid_alive = IsProcessAlive(pid);
    try {
        auto stats = KernelRequest(adapter, "/api/stats");
        Object status{{"running", Value(true)}, {"stats", stats}};
        if (pid_alive) {
            status["pid"] = Value(static_cast<double>(pid));
            status["managed"] = Value(true);
        }
        return status;
    } catch (const std::exception& ex) {
        if (pid_alive) {
            return Object{
                {"running", Value(true)},
                {"starting", Value(true)},
                {"pid", Value(static_cast<double>(pid))},
                {"managed", Value(true)},
                {"error", Value(std::string(ex.what()))},
                {"stderr", Value(ReadRecentLog(kernel_stderr_path_))},
            };
        }
        return Object{
            {"running", Value(false)},
            {"error", Value(std::string(ex.what()))},
            {"stderr", Value(ReadRecentLog(kernel_stderr_path_))},
        };
    }
}

void PanelServer::EnsureProxyCoreRunning() const {
    const auto adapter = LoadAdapter();
    if (GetOptionalBool(GetProxyCoreStatus(adapter), "running", false)) return;
    StartProxyCore(adapter);
}

Object PanelServer::StartProxyCore(const Object& adapter) const {
    SyncManagementConfig(adapter);
    StopProxyCore(adapter);
    const auto cmd = ResolveStartCommand(adapter);
    if (cmd.empty() || !fs::exists(cmd.front())) {
        return Object{{"running", Value(false)}, {"error", Value(std::string("内核文件不存在"))}};
    }

    const int stdout_fd = ::open(kernel_stdout_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int stderr_fd = ::open(kernel_stderr_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) throw std::runtime_error("failed to open proxy log files");

    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::dup2(stdout_fd, STDOUT_FILENO);
        ::dup2(stderr_fd, STDERR_FILENO);
        ::close(stdout_fd);
        ::close(stderr_fd);
        std::vector<std::string> mutable_cmd = cmd;
        std::vector<char*> argv;
        for (auto& token : mutable_cmd) argv.push_back(token.data());
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        _exit(127);
    }

    ::close(stdout_fd);
    ::close(stderr_fd);
    if (pid < 0) throw std::runtime_error("failed to fork proxy core");
    WriteKernelPid(kernel_pid_path_, pid);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    std::string last_error;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            ClearKernelPid(kernel_pid_path_);
            return Object{
                {"running", Value(false)},
                {"error", Value(std::string("内核启动失败，进程已退出"))},
                {"stderr", Value(ReadRecentLog(kernel_stderr_path_))},
            };
        }
        try {
            auto stats = KernelRequest(adapter, "/api/stats");
            return Object{
                {"running", Value(true)},
                {"stats", stats},
                {"pid", Value(static_cast<double>(pid))},
                {"managed", Value(true)},
            };
        } catch (const std::exception& ex) {
            last_error = ex.what();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    const bool alive = IsProcessAlive(pid);
    return Object{
        {"running", Value(alive)},
        {"starting", Value(alive)},
        {"pid", Value(static_cast<double>(pid))},
        {"managed", Value(true)},
        {"error", Value(last_error.empty() ? std::string("内核仍在启动中") : last_error)},
        {"stderr", Value(ReadRecentLog(kernel_stderr_path_))},
    };
}

void PanelServer::SchedulePanelRestart(int exit_code, std::chrono::seconds delay) {
    std::thread([exit_code, delay]() {
        std::this_thread::sleep_for(delay);
        std::exit(exit_code);
    }).detach();
}

std::pair<std::string, int> PanelServer::ParseBaseUrl(const std::string& base_url) {
    std::string remainder = StartsWith(base_url, "http://") ? base_url.substr(7) : base_url;
    if (const auto slash = remainder.find('/'); slash != std::string::npos) remainder = remainder.substr(0, slash);
    if (const auto colon = remainder.rfind(':'); colon != std::string::npos) {
        return {remainder.substr(0, colon), std::stoi(remainder.substr(colon + 1))};
    }
    return {remainder, 80};
}

int PanelServer::Connect(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        throw std::runtime_error("内核连接失败: getaddrinfo failed");
    }
    int fd = -1;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    if (fd < 0) throw std::runtime_error("内核连接失败: connection refused");
    return fd;
}

KernelHttpResponse PanelServer::ReadHttpResponse(int fd) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
        if (rc <= 0) break;
        data.append(buffer, static_cast<std::size_t>(rc));
    }
    if (header_end == std::string::npos) throw std::runtime_error("内核连接失败: invalid response");

    KernelHttpResponse response;
    std::istringstream stream(data.substr(0, header_end));
    std::string status_line;
    std::getline(stream, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    std::istringstream status_stream(status_line);
    std::string version;
    status_stream >> version >> response.status;
    std::getline(status_stream, response.reason);
    response.reason = Trim(response.reason);

    std::size_t content_length = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        response.headers[ToLower(Trim(line.substr(0, pos)))] = Trim(line.substr(pos + 1));
        if (ToLower(Trim(line.substr(0, pos))) == "content-length") content_length = static_cast<std::size_t>(std::stoul(Trim(line.substr(pos + 1))));
    }

    response.body = data.substr(header_end + 4);
    while (response.body.size() < content_length) {
        const ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
        if (rc <= 0) break;
        response.body.append(buffer, static_cast<std::size_t>(rc));
    }
    return response;
}

Value PanelServer::KernelRequest(const Object& adapter, const std::string& path, const std::string& method,
                                 const std::optional<std::string>& body) const {
    const auto [host, port] = ParseBaseUrl(GetOptionalString(adapter, "managementApiBase").value_or("http://127.0.0.1:9090"));
    const std::string token = GetOptionalString(adapter, "managementApiToken").value_or("");
    const int fd = Connect(host, port);

    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << ":" << port << "\r\n";
    request << "X-Admin-Token: " << token << "\r\n";
    request << "Connection: close\r\n";
    if (body.has_value()) {
        request << "Content-Type: application/json\r\n";
        request << "Content-Length: " << body->size() << "\r\n";
    }
    request << "\r\n";
    if (body.has_value()) request << *body;

    SendAll(fd, request.str());
    const auto response = ReadHttpResponse(fd);
    ::close(fd);
    if (response.status >= 400) throw std::runtime_error("内核拒绝请求 (" + std::to_string(response.status) + "): " + (response.body.empty() ? response.reason : response.body));
    if (Trim(response.body).empty()) return Value(Object{});
    return tray_panel::json::Parse(response.body);
}

std::optional<std::string> PanelServer::GetQueryParam(const std::string& query, const std::string& key) {
    std::istringstream stream(query);
    std::string item;
    while (std::getline(stream, item, '&')) {
        const auto pos = item.find('=');
        const auto current_key = pos == std::string::npos ? item : item.substr(0, pos);
        if (current_key == key) return pos == std::string::npos ? "" : item.substr(pos + 1);
    }
    return std::nullopt;
}

std::string PanelServer::Join(const std::vector<std::string>& items, const std::string& delimiter) {
    std::ostringstream out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << delimiter;
        out << items[i];
    }
    return out.str();
}

std::string PanelServer::Base64Encode(const std::string& input) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    int value = 0;
    int bits = -6;
    for (unsigned char ch : input) {
        value = (value << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            output.push_back(table[(value >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) output.push_back(table[((value << 8) >> (bits + 8)) & 0x3F]);
    while (output.size() % 4 != 0) output.push_back('=');
    return output;
}

HttpResponse PanelServer::NotFound() const {
    return JsonResponse(404, "Not Found", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("not found"))}}));
}

HttpResponse PanelServer::ServeStatic(const std::string& clean_path) {
    std::string relative = clean_path == "/" ? "/index.html" : clean_path;
    if (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (relative.find("..") != std::string::npos) return NotFound();
    const fs::path file_path = public_root_ / fs::path(relative);
    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) return NotFound();
    HttpResponse response;
    response.status = 200;
    response.reason = "OK";
    response.headers.push_back({"Content-Type", MimeType(file_path)});
    response.body = ReadFile(file_path);
    return response;
}

HttpResponse PanelServer::HandleSubscription(const HttpRequest& request) {
    const auto slug = UrlDecode(request.path.substr(std::string("/sub/").size()));
    const auto store = LoadExportUsers();
    const auto users = GetArray(store, "users");
    const Object* target_user = nullptr;
    for (const auto& item : users) {
        if (!item.IsObject()) continue;
        if (GetOptionalString(item.AsObject(), "slug").value_or("") == slug) {
            target_user = &item.AsObject();
            break;
        }
    }
    if (!target_user) return NotFound();

    const auto adapter = LoadAdapter();
    const auto runtime = KernelRequest(adapter, "/api/config").AsObject();
    const auto runtime_inbounds = GetArray(runtime, "inbounds");
    const auto fmt = GetQueryParam(request.query, "format").value_or("base64");

    std::vector<std::string> nodes;
    for (const auto& tag : ExtractInboundTags(*target_user)) {
        for (const auto& inbound_item : runtime_inbounds) {
            if (!inbound_item.IsObject()) continue;
            const auto& inbound = inbound_item.AsObject();
            if (GetOptionalString(inbound, "tag").value_or("") != tag) continue;
            std::string host = GetOptionalString(inbound, "listen").value_or("127.0.0.1");
            if (host == "0.0.0.0" || host == "::") {
                host = ResolvePublicHost(request, static_cast<int>(GetOptionalNumber(LoadSettings(), "port", 8090)));
                const auto pos = host.find(':');
                if (pos != std::string::npos) host = host.substr(0, pos);
            }
            std::string auth;
            if (GetOptionalBool(inbound, "authEnabled", false)) {
                auth = GetOptionalString(inbound, "username").value_or("") + ":" + GetOptionalString(inbound, "password").value_or("") + "@";
            }
            nodes.push_back(GetOptionalString(inbound, "protocol").value_or("http") + "://" + auth + host + ":" +
                            std::to_string(static_cast<int>(GetOptionalNumber(inbound, "port", 0))) + "#" +
                            GetOptionalString(inbound, "tag").value_or(""));
        }
    }

    if (fmt == "json") {
        Array links;
        for (const auto& node : nodes) links.push_back(Value(node));
        return JsonResponse(200, "OK", Value(Object{
            {"ok", Value(true)},
            {"subscription", Value(Object{
                {"name", Value(GetOptionalString(*target_user, "name").value_or(""))},
                {"description", Value(GetOptionalString(*target_user, "description").value_or(""))},
                {"group", Value(GetOptionalString(*target_user, "group").value_or(""))},
                {"expiresAt", Value(GetOptionalString(*target_user, "expiresAt").value_or(""))},
                {"trafficLimitGiB", Value(GetOptionalNumber(*target_user, "trafficLimitGiB", 0.0))},
                {"links", Value(links)},
            })},
        }));
    }

    HttpResponse response;
    response.status = 200;
    response.reason = "OK";
    response.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
    response.body = Base64Encode(Join(nodes, "\n"));
    return response;
}

HttpResponse PanelServer::HandleGet(const HttpRequest& request) {
    if (request.path == "/api/session") {
        const auto auth = LoadPanelAuth();
        const auto session = GetAuthenticatedSession(request, auth);
        if (session.has_value()) {
            return JsonResponse(200, "OK", Value(Object{
                {"ok", Value(true)},
                {"authenticated", Value(true)},
                {"username", Value(GetOptionalString(*session, "user").value_or(""))},
                {"settings", Value(LoadSettings())},
            }));
        }
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"authenticated", Value(false)}}));
    }

    if (StartsWith(request.path, "/api/")) {
        RequireAuth(request);
        const auto adapter = LoadAdapter();
        if (request.path == "/api/status") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"status", Value(GetProxyCoreStatus(adapter))}}));
        if (request.path == "/api/runtime/config") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"runtime", KernelRequest(adapter, "/api/config")}}));
        if (request.path == "/api/export/users") {
            auto store = LoadExportUsers();
            auto users = GetArray(store, "users");
            const int panel_port = static_cast<int>(GetOptionalNumber(LoadSettings(), "port", 8090));
            const auto host_with_port = ResolvePublicHost(request, panel_port);
            for (auto& user_item : users) {
                if (!user_item.IsObject()) continue;
                auto& user = const_cast<Object&>(user_item.AsObject());
                const auto encoded_slug = UrlEncode(GetOptionalString(user, "slug").value_or(""));
                user["subscriptionUrl"] = Value(std::string("http://") + host_with_port + "/sub/" + encoded_slug);
                user["subscriptionRawUrl"] = Value(std::string("http://") + host_with_port + "/sub/" + encoded_slug);
                user["subscriptionJsonUrl"] = Value(std::string("http://") + host_with_port + "/sub/" + encoded_slug + "?format=json");
                user["inboundCount"] = Value(static_cast<double>(ExtractInboundTags(user).size()));
            }
            return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"users", Value(users)}}));
        }
    }

    if (StartsWith(request.path, "/sub/")) return HandleSubscription(request);
    return NotFound();
}

HttpResponse PanelServer::HandlePost(const HttpRequest& request) {
    if (request.path == "/api/login") {
        const auto body = ParseObjectBody(request.body);
        const auto auth = LoadPanelAuth();
        if (GetOptionalString(body, "username").value_or("") == GetOptionalString(auth, "username").value_or("admin") &&
            GetOptionalString(body, "password").value_or("") == GetOptionalString(auth, "password").value_or("admin")) {
            auto next_auth = auth;
            auto sessions = GetObject(next_auth, "sessions");
            const auto sid = GenerateToken();
            sessions[sid] = Value(Object{{"user", Value(GetOptionalString(auth, "username").value_or("admin"))}, {"created", Value(static_cast<double>(std::time(nullptr)))}}); 
            next_auth["sessions"] = Value(sessions);
            SavePanelAuth(next_auth);
            auto response = JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"session_id", Value(sid)}}));
            response.headers.push_back({"Set-Cookie", "session_id=" + sid + "; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax"});
            return response;
        }
        return JsonResponse(401, "Unauthorized", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("账号或密码错误"))}}));
    }

    RequireAuth(request);
    const auto adapter = LoadAdapter();

    if (request.path == "/api/auth/change") {
        auto auth = LoadPanelAuth();
        const auto body = ParseObjectBody(request.body);
        if (GetOptionalString(body, "currentPassword").value_or("") != GetOptionalString(auth, "password").value_or("admin")) {
            return JsonResponse(400, "Bad Request", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("当前密码错误"))}}));
        }
        auth["username"] = Value(GetOptionalString(body, "newUsername").value_or(GetOptionalString(auth, "username").value_or("admin")));
        auth["password"] = Value(GetOptionalString(body, "newPassword").value_or(""));
        auth["sessions"] = Value(Object{});
        SavePanelAuth(auth);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }

    if (request.path == "/api/settings") {
        auto merged = LoadSettings();
        const auto current = merged;
        const auto body = ParseObjectBody(request.body);
        for (const auto& [key, value] : body) merged[key] = value;
        merged = NormalizePanelSettings(merged);
        SaveSettings(merged);
        const bool restart_required = GetOptionalString(current, "host").value_or("0.0.0.0") != GetOptionalString(merged, "host").value_or("0.0.0.0") ||
            static_cast<int>(GetOptionalNumber(current, "port", 8090)) != static_cast<int>(GetOptionalNumber(merged, "port", 8090));
        Object payload{{"ok", Value(true)}, {"settings", Value(merged)}, {"restartRequired", Value(restart_required)}};
        if (restart_required) {
            payload["redirectUrl"] = Value(std::string("http://") + ResolvePanelRedirectHost(request, GetOptionalString(merged, "host").value_or("0.0.0.0")) +
                                           ":" + std::to_string(static_cast<int>(GetOptionalNumber(merged, "port", 8090))) + "/");
            auto response = JsonResponse(200, "OK", Value(payload));
            SchedulePanelRestart(1, std::chrono::seconds(1));
            return response;
        }
        return JsonResponse(200, "OK", Value(payload));
    }

    if (request.path == "/api/logout") {
        auto auth = LoadPanelAuth();
        auto sessions = GetObject(auth, "sessions");
        sessions.erase(GetSessionId(request));
        auth["sessions"] = Value(sessions);
        SavePanelAuth(auth);
        auto response = JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
        response.headers.push_back({"Set-Cookie", "session_id=; Path=/; Max-Age=0; HttpOnly"});
        return response;
    }

    if (request.path == "/api/restart") {
        auto response = JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
        SchedulePanelRestart(1, std::chrono::seconds(1));
        return response;
    }

    if (request.path == "/api/start") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"status", Value(StartProxyCore(adapter))}}));
    if (request.path == "/api/runtime/inbounds") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"result", KernelRequest(adapter, "/api/inbounds", "POST", request.body)}}));
    if (request.path == "/api/runtime/outbounds") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"result", KernelRequest(adapter, "/api/outbounds", "POST", request.body)}}));
    if (request.path == "/api/runtime/routes") return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}, {"result", KernelRequest(adapter, "/api/routing/rules", "POST", request.body)}}));

    if (request.path == "/api/export/users") {
        auto store = LoadExportUsers();
        auto users = GetArray(store, "users");
        auto body = ParseObjectBody(request.body);
        const auto name = Trim(GetOptionalString(body, "name").value_or(""));
        if (name.empty()) return JsonResponse(400, "Bad Request", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("名称不能为空"))}}));
        body["name"] = Value(name);
        body["slug"] = Value(EnsureUniqueSlug(users, GetOptionalString(body, "slug").value_or(name)));
        body["id"] = Value(GenerateToken(16));
        body["enabled"] = Value(GetOptionalBool(body, "enabled", true));
        body["trafficLimitGiB"] = Value(GetOptionalNumber(body, "trafficLimitGiB", 0.0));
        body["inboundTags"] = Value(NormalizeInboundTags(body));
        users.push_back(Value(body));
        store["users"] = Value(users);
        SaveExportUsers(store);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }

    return NotFound();
}

HttpResponse PanelServer::HandlePut(const HttpRequest& request) {
    RequireAuth(request);
    const auto adapter = LoadAdapter();
    const auto body = ParseObjectBody(request.body);
    if (StartsWith(request.path, "/api/runtime/inbounds/")) {
        const auto tag = request.path.substr(std::string("/api/runtime/inbounds/").size());
        KernelRequest(adapter, "/api/inbounds/" + tag, "DELETE");
        KernelRequest(adapter, "/api/inbounds", "POST", request.body);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/runtime/outbounds/")) {
        const auto tag = request.path.substr(std::string("/api/runtime/outbounds/").size());
        KernelRequest(adapter, "/api/outbounds/" + tag, "DELETE");
        KernelRequest(adapter, "/api/outbounds", "POST", request.body);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/runtime/routes/")) {
        const auto tag = request.path.substr(std::string("/api/runtime/routes/").size());
        KernelRequest(adapter, "/api/routing/rules/" + tag, "DELETE");
        KernelRequest(adapter, "/api/routing/rules", "POST", request.body);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/export/users/")) {
        const auto uid = request.path.substr(std::string("/api/export/users/").size());
        auto store = LoadExportUsers();
        auto users = GetArray(store, "users");
        for (auto& item : users) {
            if (!item.IsObject()) continue;
            auto& user = const_cast<Object&>(item.AsObject());
            if (GetOptionalString(user, "id").value_or("") != uid) continue;
            for (const auto& [key, value] : body) user[key] = value;
            const auto name = Trim(GetOptionalString(user, "name").value_or(""));
            if (name.empty()) return JsonResponse(400, "Bad Request", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("名称不能为空"))}}));
            user["name"] = Value(name);
            user["slug"] = Value(EnsureUniqueSlug(users, GetOptionalString(user, "slug").value_or(name), uid));
            user["enabled"] = Value(GetOptionalBool(user, "enabled", true));
            user["trafficLimitGiB"] = Value(GetOptionalNumber(user, "trafficLimitGiB", 0.0));
            user["inboundTags"] = Value(NormalizeInboundTags(user));
        }
        store["users"] = Value(users);
        SaveExportUsers(store);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    return NotFound();
}

HttpResponse PanelServer::HandleDelete(const HttpRequest& request) {
    RequireAuth(request);
    const auto adapter = LoadAdapter();
    if (StartsWith(request.path, "/api/runtime/inbounds/")) {
        KernelRequest(adapter, "/api/inbounds/" + request.path.substr(std::string("/api/runtime/inbounds/").size()), "DELETE");
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/runtime/outbounds/")) {
        KernelRequest(adapter, "/api/outbounds/" + request.path.substr(std::string("/api/runtime/outbounds/").size()), "DELETE");
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/runtime/routes/")) {
        KernelRequest(adapter, "/api/routing/rules/" + request.path.substr(std::string("/api/runtime/routes/").size()), "DELETE");
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    if (StartsWith(request.path, "/api/export/users/")) {
        const auto uid = request.path.substr(std::string("/api/export/users/").size());
        auto store = LoadExportUsers();
        const auto users = GetArray(store, "users");
        Array next;
        for (const auto& item : users) {
            if (!item.IsObject() || GetOptionalString(item.AsObject(), "id").value_or("") != uid) next.push_back(item);
        }
        store["users"] = Value(next);
        SaveExportUsers(store);
        return JsonResponse(200, "OK", Value(Object{{"ok", Value(true)}}));
    }
    return NotFound();
}

HttpResponse PanelServer::Route(const HttpRequest& request) {
    if (!StartsWith(request.path, "/api/") && !StartsWith(request.path, "/sub/")) return ServeStatic(request.path);
    if (request.method == "GET") return HandleGet(request);
    if (request.method == "POST") return HandlePost(request);
    if (request.method == "PUT") return HandlePut(request);
    if (request.method == "DELETE") return HandleDelete(request);
    return JsonResponse(405, "Method Not Allowed", Value(Object{{"ok", Value(false)}, {"error", Value(std::string("unsupported method"))}}));
}

void PanelServer::HandleClient(int client_fd) {
    try {
        const auto request = ReadRequest(client_fd);
        const auto response = Route(request);
        WriteResponse(client_fd, response);
    } catch (const std::exception& ex) {
        const auto response = JsonResponse(500, "Internal Server Error", Value(Object{{"ok", Value(false)}, {"error", Value(std::string(ex.what()))}}));
        WriteResponse(client_fd, response);
    }
    ::close(client_fd);
}

int PanelServer::Run() {
    ::signal(SIGPIPE, SIG_IGN);
    fs::create_directories(runtime_root_);
    fs::create_directories(app_root_ / "data");
    EnsureProxyCoreRunning();
    const auto settings = LoadSettings();
    const auto host = cli_host_.value_or(GetOptionalString(settings, "host").value_or("0.0.0.0"));
    const int port = cli_port_.value_or(static_cast<int>(GetOptionalNumber(settings, "port", 8090)));
    const int server_fd = CreateListener(host, port);
    std::cout << "Panel running at http://" << host << ":" << port << std::endl;
    while (true) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&address), &length);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        std::thread(&PanelServer::HandleClient, this, client_fd).detach();
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    fs::path app_root = fs::current_path();
    std::optional<std::string> host;
    std::optional<int> port;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-root" && i + 1 < argc) app_root = fs::path(argv[++i]);
        else if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else {
            std::cerr << "usage: tray_web_server [--app-root path] [--host host] [--port port]" << std::endl;
            return 1;
        }
    }

    try {
        PanelServer server(std::move(app_root), std::move(host), std::move(port));
        return server.Run();
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return 1;
    }
}

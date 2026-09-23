#include <winsock2.h>
#include <ws2tcpip.h>
#include "web_server.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool send_all(SOCKET socket, const char *data, size_t length) {
    while (length) {
        int sent = send(socket, data, length > INT_MAX ? INT_MAX : (int)length, 0);
        if (sent <= 0) return false;
        data += sent; length -= (size_t)sent;
    }
    return true;
}

static void send_response(SOCKET socket, int code, const char *status,
                          const char *content_type, const char *body, size_t length) {
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        code, status, content_type, length);
    send_all(socket, header, (size_t)n);
    if (length) send_all(socket, body, length);
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (_stricmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (_stricmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (_stricmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
    if (_stricmp(ext, ".png") == 0) return "image/png";
    return "application/octet-stream";
}

static void serve_file(SOCKET socket, const char *file_path) {
    size_t length = 0;
    char *body = read_entire_file(file_path, &length);
    if (!body) {
        const char *missing = "Not Found";
        send_response(socket, 404, "Not Found", "text/plain; charset=utf-8", missing, strlen(missing));
        return;
    }
    send_response(socket, 200, "OK", mime_type(file_path), body, length);
    free(body);
}

static size_t content_length(const char *request) {
    const char *p = request;
    while ((p = strstr(p, "\n")) != NULL) {
        ++p;
        if (_strnicmp(p, "Content-Length:", 15) == 0) return (size_t)strtoull(p + 15, NULL, 10);
    }
    return 0;
}

static void command_response(SOCKET socket, AppState *state, const char *body) {
    char command[512] = "", result[2048] = "";
    bool parsed = json_get_string(body, "command", command, sizeof(command));
    bool ok = parsed && app_state_command(state, command, result, sizeof(result));
    if (!parsed) snprintf(result, sizeof(result), "请求格式不正确");
    StrBuf json; sb_init(&json, 1024);
    sb_printf(&json, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    sb_json_string(&json, result);
    sb_printf(&json, ",\"changed\":%s}", ok ? "true" : "false");
    send_response(socket, ok ? 200 : 400, ok ? "OK" : "Bad Request",
                  "application/json; charset=utf-8", json.data, json.len);
    sb_free(&json);
}

static void handle_client(SOCKET client, AppState *state) {
    StrBuf request; sb_init(&request, 8192);
    size_t expected_body = 0;
    for (;;) {
        char buffer[8192];
        int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        sb_append_n(&request, buffer, (size_t)got);
        char *headers_end = request.data ? strstr(request.data, "\r\n\r\n") : NULL;
        if (headers_end) {
            expected_body = content_length(request.data);
            size_t header_size = (size_t)(headers_end + 4 - request.data);
            if (request.len >= header_size + expected_body) break;
        }
        if (request.len > 65536) break;
    }
    if (!request.data) return;
    char method[16] = "", path[512] = "";
    sscanf(request.data, "%15s %511s", method, path);
    char *query = strchr(path, '?'); if (query) *query = '\0';
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/control") == 0))
        serve_file(client, "static/control.html");
    else if (strcmp(method, "GET") == 0 &&
             (strcmp(path, "/console") == 0 || strcmp(path, "/overlay") == 0))
        serve_file(client, "static/overlay.html");
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/state") == 0) {
        size_t length = 0; char *json = app_state_snapshot(state, &length);
        if (json) { send_response(client, 200, "OK", "application/json; charset=utf-8", json, length); free(json); }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/command") == 0) {
        char *body = strstr(request.data, "\r\n\r\n");
        command_response(client, state, body ? body + 4 : "");
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/static/", 8) == 0) {
        const char *name = path + 8;
        bool safe = strcmp(name, "control.css") == 0 || strcmp(name, "control.js") == 0 ||
                    strcmp(name, "overlay.css") == 0 || strcmp(name, "overlay.js") == 0 ||
                    strcmp(name, "assets/shorekeeper-queue-frame.png") == 0;
        if (safe) { char file[600]; snprintf(file, sizeof(file), "static/%s", name); serve_file(client, file); }
        else send_response(client, 404, "Not Found", "text/plain", "Not Found", 9);
    } else send_response(client, 404, "Not Found", "text/plain", "Not Found", 9);
    sb_free(&request);
}

DWORD WINAPI web_server_thread(LPVOID parameter) {
    WebServerArgs args = *(WebServerArgs *)parameter;
    free(parameter);
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { WSACleanup(); return 1; }
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)args.port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        printf("[网页] 无法监听 127.0.0.1:%d，端口可能已被占用。\n", args.port);
        closesocket(listener); WSACleanup(); return 1;
    }
    printf("[网页] 管理页面已启动：http://127.0.0.1:%d/\n", args.port);
    printf("[网页] OBS 透明队列：http://127.0.0.1:%d/console\n", args.port);
    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        handle_client(client, args.state);
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
}

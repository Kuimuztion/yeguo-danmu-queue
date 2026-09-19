#define _WIN32_WINNT 0x0602
#include "bilibili.h"
#include "util.h"

#include <bcrypt.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const int MIXIN_TAB[64] = {
    46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,
    27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13,
    37,48,7,16,24,55,40,61,26,17,0,1,60,51,30,4,
    22,25,54,21,56,59,6,63,57,62,11,36,20,34,44,52
};

typedef struct {
    HINTERNET socket;
    volatile LONG running;
} HeartbeatContext;

typedef int (__cdecl *ZlibUncompress)(unsigned char *, unsigned long *,
                                      const unsigned char *, unsigned long);

static bool decompress_zlib(const unsigned char *source, size_t source_len,
                            unsigned char **output, size_t *output_len) {
    static HMODULE module = NULL;
    static ZlibUncompress uncompress_fn = NULL;
    if (!module) {
        module = LoadLibraryW(L"zlib1.dll");
        if (module) {
            FARPROC procedure = GetProcAddress(module, "uncompress");
            if (procedure) memcpy(&uncompress_fn, &procedure, sizeof(uncompress_fn));
        }
    }
    if (!uncompress_fn || source_len > 0xFFFFFFFFu) return false;
    unsigned long capacity = 256 * 1024;
    for (int attempt = 0; attempt < 5; ++attempt) {
        unsigned char *buffer = (unsigned char *)malloc(capacity);
        if (!buffer) return false;
        unsigned long actual = capacity;
        int code = uncompress_fn(buffer, &actual, source, (unsigned long)source_len);
        if (code == 0) {
            *output = buffer; *output_len = actual; return true;
        }
        free(buffer);
        if (code != -5) return false; /* Z_BUF_ERROR */
        capacity *= 2;
    }
    return false;
}

static wchar_t *wide(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *out = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
    if (out) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, n);
    return out;
}

static char *http_get(HINTERNET session, const char *host, const char *path,
                      const char *cookie, DWORD *status_out) {
    wchar_t *whost = wide(host), *wpath = wide(path);
    if (!whost || !wpath) { free(whost); free(wpath); return NULL; }
    HINTERNET connect = WinHttpConnect(session, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    free(whost); free(wpath);
    if (!request) { if (connect) WinHttpCloseHandle(connect); return NULL; }
    wchar_t headers[2048];
    if (cookie && *cookie) {
        wchar_t *wcookie = wide(cookie);
        _snwprintf(headers, 2048,
            L"Origin: https://live.bilibili.com\r\nReferer: https://live.bilibili.com/\r\nCookie: %ls\r\n", wcookie);
        free(wcookie);
    } else {
        wcscpy(headers, L"Origin: https://live.bilibili.com\r\nReferer: https://live.bilibili.com/\r\n");
    }
    BOOL ok = WinHttpSendRequest(request, headers, (DWORD)-1L,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, NULL);
    DWORD status = 0, status_size = sizeof(status);
    if (ok) WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &status, &status_size, NULL);
    if (status_out) *status_out = status;
    StrBuf body; sb_init(&body, 8192);
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        char *chunk = (char *)malloc(available);
        DWORD read = 0;
        if (!chunk || !WinHttpReadData(request, chunk, available, &read)) {
            free(chunk); ok = FALSE; break;
        }
        sb_append_n(&body, chunk, read);
        free(chunk);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    if (!ok || status < 200 || status >= 300) { sb_free(&body); return NULL; }
    return body.data;
}

static bool md5_hex(const char *text, char output[33]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD object_size = 0, hash_size = 0, got = 0;
    unsigned char digest[16];
    unsigned char *object = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, NULL, 0);
    if (status < 0) goto done;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                               sizeof(object_size), &got, 0);
    if (status < 0) goto done;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_size,
                               sizeof(hash_size), &got, 0);
    if (status < 0 || hash_size != sizeof(digest)) goto done;
    object = (unsigned char *)malloc(object_size);
    if (!object) goto done;
    status = BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0);
    if (status < 0) goto done;
    status = BCryptHashData(hash, (PUCHAR)text, (ULONG)strlen(text), 0);
    if (status < 0) goto done;
    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (status < 0) goto done;
    for (int i = 0; i < 16; ++i) sprintf(output + i * 2, "%02x", digest[i]);
    output[32] = '\0';
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    free(object);
    return status >= 0;
}

static void file_key(const char *url, char *out, size_t size) {
    const char *slash = strrchr(url, '/');
    const char *start = slash ? slash + 1 : url;
    const char *dot = strchr(start, '.');
    size_t len = dot ? (size_t)(dot - start) : strlen(start);
    if (len >= size) len = size - 1;
    memcpy(out, start, len); out[len] = '\0';
}

static void cookie_value(const char *cookie, const char *key, char *out, size_t out_size) {
    char pattern[128]; snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(cookie, pattern);
    if (!p) return;
    p += strlen(pattern);
    const char *end = strchr(p, ';');
    size_t length = end ? (size_t)(end - p) : strlen(p);
    if (length >= out_size) length = out_size - 1;
    memcpy(out, p, length); out[length] = '\0';
}

static bool discover(HINTERNET session, AppState *state,
                     char *buvid, size_t buvid_size,
                     char *host, size_t host_size, int *port,
                     char *token, size_t token_size, long long *real_room) {
    DWORD status = 0;
    long long requested_room = state->room_id;
    char buvid4[256] = "";
    char cookie[4608] = "";
    if (*state->auth_cookie) {
        snprintf(cookie, sizeof(cookie), "%s", state->auth_cookie);
        if (*state->auth_buvid) snprintf(buvid, buvid_size, "%s", state->auth_buvid);
        else cookie_value(cookie, "buvid3", buvid, buvid_size);
    } else {
        char *spi = http_get(session, "api.bilibili.com", "/x/frontend/finger/spi", NULL, &status);
        if (spi) {
            json_get_string(spi, "b_3", buvid, buvid_size);
            json_get_string(spi, "b_4", buvid4, sizeof(buvid4));
            free(spi);
        }
        if (!*buvid) snprintf(buvid, buvid_size, "00000000-0000-4000-8000-%012llxinfoc", (unsigned long long)time(NULL));
        snprintf(cookie, sizeof(cookie), "buvid3=%s; buvid4=%s; CURRENT_FNVAL=4048", buvid, buvid4);
    }

    char room_path[256];
    snprintf(room_path, sizeof(room_path), "/room/v1/Room/room_init?id=%lld", requested_room);
    char *room = http_get(session, "api.live.bilibili.com", room_path, cookie, &status);
    if (!room) return false;
    *real_room = json_get_integer(room, "room_id", requested_room);
    free(room);

    char *nav = http_get(session, "api.bilibili.com", "/x/web-interface/nav", cookie, &status);
    if (!nav) return false;
    char img_url[512] = "", sub_url[512] = "";
    json_get_string(nav, "img_url", img_url, sizeof(img_url));
    json_get_string(nav, "sub_url", sub_url, sizeof(sub_url));
    free(nav);
    if (!*img_url || !*sub_url) return false;
    char img[128], sub[128], source[256], mixin[33];
    file_key(img_url, img, sizeof(img)); file_key(sub_url, sub, sizeof(sub));
    snprintf(source, sizeof(source), "%s%s", img, sub);
    size_t source_len = strlen(source);
    int used = 0;
    for (int i = 0; i < 64 && used < 32; ++i)
        if ((size_t)MIXIN_TAB[i] < source_len) mixin[used++] = source[MIXIN_TAB[i]];
    mixin[used] = '\0';

    long long wts = (long long)time(NULL);
    char query[512], signed_text[600], rid[33], danmu_path[768];
    snprintf(query, sizeof(query), "id=%lld&type=0&web_location=444.8&wts=%lld", *real_room, wts);
    snprintf(signed_text, sizeof(signed_text), "%s%s", query, mixin);
    if (!md5_hex(signed_text, rid)) return false;
    snprintf(danmu_path, sizeof(danmu_path),
             "/xlive/web-room/v1/index/getDanmuInfo?%s&w_rid=%s", query, rid);
    char *danmu = http_get(session, "api.live.bilibili.com", danmu_path, cookie, &status);
    if (!danmu) return false;
    long long code = json_get_integer(danmu, "code", -999);
    bool ok = code == 0 && json_get_string(danmu, "token", token, token_size) &&
              json_get_string(danmu, "host", host, host_size);
    *port = (int)json_get_integer(danmu, "wss_port", 443);
    if (!ok) printf("[B站] getDanmuInfo 返回 code=%lld\n", code);
    free(danmu);
    return ok;
}

static void put_be16(unsigned char *p, uint16_t n) { p[0] = (unsigned char)(n >> 8); p[1] = (unsigned char)n; }
static void put_be32(unsigned char *p, uint32_t n) {
    p[0] = (unsigned char)(n >> 24); p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8); p[3] = (unsigned char)n;
}
static uint16_t get_be16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static unsigned char *make_packet(uint32_t operation, const char *body, size_t *length) {
    size_t body_len = body ? strlen(body) : 0;
    *length = 16 + body_len;
    unsigned char *packet = (unsigned char *)malloc(*length);
    if (!packet) return NULL;
    put_be32(packet, (uint32_t)*length); put_be16(packet + 4, 16); put_be16(packet + 6, 1);
    put_be32(packet + 8, operation); put_be32(packet + 12, 1);
    if (body_len) memcpy(packet + 16, body, body_len);
    return packet;
}

static void token_to_id(const char *token, size_t length, char *out, size_t out_size) {
    while (length && (*token == ' ' || *token == '\t')) { ++token; --length; }
    if (length >= out_size) length = out_size - 1;
    memcpy(out, token, length); out[length] = '\0';
}

static void handle_event(AppState *state, const char *json) {
    char command[128] = "";
    if (!json_get_string(json, "cmd", command, sizeof(command))) return;
    char *colon = strchr(command, ':'); if (colon) *colon = '\0';
    char message[512];
    if (strcmp(command, "DANMU_MSG") == 0) {
        const char *info = json_find_value(json, "info");
        const char *text_token = NULL, *user_array = NULL, *uid_token = NULL, *name_token = NULL;
        size_t text_len = 0, user_len = 0, uid_len = 0, name_len = 0;
        char text[256] = "", uid[MAX_USER_ID] = "", name[MAX_USER_NAME] = "";
        if (!info || !json_array_element(info, 1, &text_token, &text_len) ||
            !json_array_element(info, 2, &user_array, &user_len) ||
            !json_array_element(user_array, 0, &uid_token, &uid_len) ||
            !json_array_element(user_array, 1, &name_token, &name_len)) return;
        json_token_string(text_token, text_len, text, sizeof(text));
        token_to_id(uid_token, uid_len, uid, sizeof(uid));
        json_token_string(name_token, name_len, name, sizeof(name));
        if (strcmp(uid, "0") == 0) {
            const char *meta = NULL, *hash_token = NULL; size_t meta_len = 0, hash_len = 0;
            char hash[80] = "";
            if (json_array_element(info, 0, &meta, &meta_len) &&
                json_array_element(meta, 7, &hash_token, &hash_len) &&
                json_token_string(hash_token, hash_len, hash, sizeof(hash)) && *hash)
                snprintf(uid, sizeof(uid), "hash:%s", hash);
        }
        if (strcmp(text, state->keyword) == 0)
            app_state_join(state, uid, name, "bilibili", message, sizeof(message));
    } else if (strcmp(command, "SEND_GIFT") == 0) {
        char gift[128] = "", name[MAX_USER_NAME] = "", uid[MAX_USER_ID] = "";
        json_get_string(json, "giftName", gift, sizeof(gift));
        json_get_string(json, "uname", name, sizeof(name));
        const char *uid_value = json_find_value(json, "uid");
        if (uid_value) {
            size_t uid_len = 0; while (uid_value[uid_len] >= '0' && uid_value[uid_len] <= '9') ++uid_len;
            token_to_id(uid_value, uid_len, uid, sizeof(uid));
        }
        if (*uid && strcmp(gift, state->gift_name) == 0)
            app_state_gift(state, uid, name, message, sizeof(message));
    }
}

static void process_packets(AppState *state, const unsigned char *data, size_t length) {
    size_t offset = 0;
    while (offset + 16 <= length) {
        uint32_t packet_len = get_be32(data + offset);
        uint16_t header_len = get_be16(data + offset + 4);
        uint16_t version = get_be16(data + offset + 6);
        uint32_t operation = get_be32(data + offset + 8);
        if (packet_len < header_len || offset + packet_len > length) return;
        const unsigned char *body = data + offset + header_len;
        size_t body_len = packet_len - header_len;
        if (version == 2) {
            unsigned char *plain = NULL; size_t plain_len = 0;
            if (decompress_zlib(body, body_len, &plain, &plain_len)) {
                process_packets(state, plain, plain_len);
                free(plain);
            } else {
                app_state_set_live(state, "error", "无法解压 B 站消息，请确认 build/zlib1.dll 存在");
            }
        } else if (operation == 8) {
            app_state_set_live(state, "connected",
                state->auth_uid > 0 ? "已连接 B 站直播弹幕服务器（登录鉴权）" :
                                      "已连接 B 站直播弹幕服务器（匿名，用户名可能脱敏）");
        } else if (operation == 5 && (version == 0 || version == 1)) {
            char *json = (char *)malloc(body_len + 1);
            if (json) { memcpy(json, body, body_len); json[body_len] = '\0'; handle_event(state, json); free(json); }
        } else if (operation == 5 && version == 3) {
            app_state_set_live(state, "error", "服务器返回 Brotli 协议，等待自动重连");
        }
        offset += packet_len;
    }
}

static DWORD WINAPI heartbeat_thread(LPVOID parameter) {
    HeartbeatContext *ctx = (HeartbeatContext *)parameter;
    while (InterlockedCompareExchange(&ctx->running, 1, 1)) {
        size_t len = 0;
        unsigned char *packet = make_packet(2, "[object Object]", &len);
        if (packet) {
            WinHttpWebSocketSend(ctx->socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                 packet, (DWORD)len);
            free(packet);
        }
        for (int i = 0; i < 300 && InterlockedCompareExchange(&ctx->running, 1, 1); ++i) Sleep(100);
    }
    return 0;
}

static bool connect_stream(HINTERNET session, AppState *state, const char *host, int port,
                           const char *token, const char *buvid, long long room_id) {
    wchar_t *whost = wide(host);
    HINTERNET connect = whost ? WinHttpConnect(session, whost, (INTERNET_PORT)port, 0) : NULL;
    free(whost);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/sub", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (!request) { if (connect) WinHttpCloseHandle(connect); return false; }
    WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
    const wchar_t *headers = L"Origin: https://live.bilibili.com\r\n";
    bool ok = WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, NULL);
    HINTERNET socket = ok ? WinHttpWebSocketCompleteUpgrade(request, 0) : NULL;
    WinHttpCloseHandle(request);
    if (!socket) { WinHttpCloseHandle(connect); return false; }
    char auth[2048];
    snprintf(auth, sizeof(auth),
        "{\"uid\":%lld,\"roomid\":%lld,\"protover\":2,\"buvid\":\"%s\","
        "\"platform\":\"web\",\"type\":2,\"key\":\"%s\"}",
        state->auth_uid, room_id, buvid, token);
    size_t auth_len = 0;
    unsigned char *packet = make_packet(7, auth, &auth_len);
    if (!packet || WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                        packet, (DWORD)auth_len) != NO_ERROR) {
        free(packet); WinHttpCloseHandle(socket); WinHttpCloseHandle(connect); return false;
    }
    free(packet);
    HeartbeatContext ctx = {socket, 1};
    HANDLE heartbeat = CreateThread(NULL, 0, heartbeat_thread, &ctx, 0, NULL);
    StrBuf received; sb_init(&received, 65536);
    for (;;) {
        unsigned char chunk[65536]; DWORD read = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD error = WinHttpWebSocketReceive(socket, chunk, sizeof(chunk), &read, &type);
        if (error != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            sb_append_n(&received, (const char *)chunk, read);
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                process_packets(state, (const unsigned char *)received.data, received.len);
                received.len = 0; if (received.data) received.data[0] = '\0';
            }
        }
    }
    InterlockedExchange(&ctx.running, 0);
    if (heartbeat) { WaitForSingleObject(heartbeat, 2000); CloseHandle(heartbeat); }
    sb_free(&received);
    WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    WinHttpCloseHandle(socket); WinHttpCloseHandle(connect);
    return false;
}

DWORD WINAPI bilibili_thread(LPVOID parameter) {
    AppState *state = (AppState *)parameter;
    HINTERNET session = WinHttpOpen(L"Bycute OBS Queue/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { app_state_set_live(state, "error", "无法初始化 WinHTTP"); return 1; }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 60000);
    for (;;) {
        app_state_set_live(state, "connecting",
            state->auth_uid > 0 ? "正在使用本地登录态连接 B 站直播间" :
                                  "正在匿名连接 B 站直播间");
        char buvid[256] = "", host[256] = "", token[1024] = "";
        int port = 443; long long room = state->room_id;
        if (!discover(session, state, buvid, sizeof(buvid), host, sizeof(host),
                      &port, token, sizeof(token), &room)) {
            app_state_set_live(state, "reconnecting", "获取弹幕令牌失败，5 秒后重试");
            Sleep(5000); continue;
        }
        char status[256]; snprintf(status, sizeof(status), "已取得匿名令牌，连接 %s:%d", host, port);
        app_state_set_live(state, "connecting", status);
        connect_stream(session, state, host, port, token, buvid, room);
        app_state_set_live(state, "reconnecting", "弹幕连接已断开，5 秒后重试");
        Sleep(5000);
    }
}

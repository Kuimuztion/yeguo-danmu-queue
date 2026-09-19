#define _WIN32_WINNT 0x0602
#include "bilibili.h"
#include "queue.h"
#include "util.h"
#include "web_server.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load_config(long long *room_id, char *keyword, size_t keyword_size,
                        char *gift, size_t gift_size, int *port,
                        int *font_size, int *max_rows) {
    *room_id = 1881592284LL;
    snprintf(keyword, keyword_size, "排队");
    snprintf(gift, gift_size, "粉丝团灯牌");
    *port = 8765; *font_size = 36; *max_rows = 50;
    size_t length = 0;
    char *json = read_entire_file("config.json", &length);
    if (!json) return;
    *room_id = json_get_integer(json, "room_id", *room_id);
    json_get_string(json, "keyword", keyword, keyword_size);
    *port = (int)json_get_integer(json, "port", *port);
    *font_size = (int)json_get_integer(json, "font_size", *font_size);
    *max_rows = (int)json_get_integer(json, "max_rows", *max_rows);
    const char *gifts = json_find_value(json, "priority_gifts");
    if (gifts && *gifts == '[') {
        const char *token = NULL; size_t token_len = 0;
        if (json_array_element(gifts, 0, &token, &token_len))
            json_token_string(token, token_len, gift, gift_size);
    }
    free(json);
}

static void load_auth(AppState *state) {
    size_t length = 0;
    char *json = read_entire_file("auth.json", &length);
    if (!json) return;
    json_get_string(json, "cookie", state->auth_cookie, sizeof(state->auth_cookie));
    json_get_string(json, "buvid", state->auth_buvid, sizeof(state->auth_buvid));
    state->auth_uid = json_get_integer(json, "uid", 0);
    free(json);
    if (*state->auth_cookie && state->auth_uid > 0)
        printf("[鉴权] 已加载本地 B 站登录态（UID %lld）\n", state->auth_uid);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    long long room_id; int port, font_size, max_rows;
    char keyword[64], gift[128];
    load_config(&room_id, keyword, sizeof(keyword), gift, sizeof(gift),
                &port, &font_size, &max_rows);
    AppState state;
    app_state_init(&state, "data/state.json", room_id, keyword, gift, font_size, max_rows);
    app_state_load(&state);
    load_auth(&state);

    WebServerArgs *web_args = (WebServerArgs *)malloc(sizeof(*web_args));
    web_args->state = &state; web_args->port = port;
    HANDLE web_thread = CreateThread(NULL, 0, web_server_thread, web_args, 0, NULL);
    HANDLE live_thread = CreateThread(NULL, 0, bilibili_thread, &state, 0, NULL);
    if (!web_thread || !live_thread) {
        fprintf(stderr, "无法启动后台线程。\n");
        return 1;
    }
    char control_url[256], overlay_url[256];
    snprintf(control_url, sizeof(control_url), "http://127.0.0.1:%d/", port);
    snprintf(overlay_url, sizeof(overlay_url), "http://127.0.0.1:%d/console", port);
    printf("\n====================================================================\n");
    printf("管理页面：%s\nOBS 透明队列：%s\nB站直播间：%lld\n", control_url, overlay_url, room_id);
    printf("====================================================================\n");
    printf("命令：del-3 / add-你好 / add-你好-3 / font-36 / list / clear / help\n");
    printf("离线测试：test-queue-我是hello / test-gift-我是hello\n\n");
    if (argc < 2 || strcmp(argv[1], "--no-browser") != 0) {
        wchar_t *url = NULL;
        int n = MultiByteToWideChar(CP_UTF8, 0, control_url, -1, NULL, 0);
        if (n > 0) {
            url = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, control_url, -1, url, n);
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            free(url);
        }
    }
    char line[1024], result[4096];
    while (printf("queue> "), fflush(stdout), fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        bool ok = app_state_command(&state, line, result, sizeof(result));
        printf("%s %s\n", ok ? "✓" : "✗", result);
    }
    return 0;
}

#include "queue.h"
#include "util.h"

#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void copy_text(char *dst, size_t size, const char *src) {
    if (!size) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static int find_id(AppState *state, const char *id) {
    for (int i = 0; i < state->queue_count; ++i)
        if (strcmp(state->queue[i].id, id) == 0) return i;
    return -1;
}

static int find_name(AppState *state, const char *name) {
    for (int i = 0; i < state->queue_count; ++i)
        if (_stricmp(state->queue[i].name, name) == 0) return i;
    return -1;
}

static int find_pending(AppState *state, const char *id) {
    for (int i = 0; i < state->pending_count; ++i)
        if (strcmp(state->pending[i].id, id) == 0) return i;
    return -1;
}

static bool masked_name_matches(const char *masked, const char *full) {
    const char *star = strchr(masked, '*');
    if (!star || star == masked) return false;
    size_t prefix = (size_t)(star - masked);
    return strncmp(masked, full, prefix) == 0;
}

static int unique_pending_name_match(AppState *state, const char *masked) {
    int found = -1;
    for (int i = 0; i < state->pending_count; ++i) {
        if (masked_name_matches(masked, state->pending[i].name)) {
            if (found >= 0) return -1;
            found = i;
        }
    }
    return found;
}

static int unique_queue_name_match(AppState *state, const char *full) {
    int found = -1;
    for (int i = 0; i < state->queue_count; ++i) {
        if (strncmp(state->queue[i].id, "hash:", 5) == 0 &&
            masked_name_matches(state->queue[i].name, full)) {
            if (found >= 0) return -1;
            found = i;
        }
    }
    return found;
}

static void remove_queue(AppState *state, int index) {
    if (index < 0 || index >= state->queue_count) return;
    memmove(&state->queue[index], &state->queue[index + 1],
            (size_t)(state->queue_count - index - 1) * sizeof(QueueUser));
    --state->queue_count;
}

static void remove_pending(AppState *state, int index) {
    if (index < 0 || index >= state->pending_count) return;
    memmove(&state->pending[index], &state->pending[index + 1],
            (size_t)(state->pending_count - index - 1) * sizeof(PendingUser));
    --state->pending_count;
}

static void save_locked(AppState *state) {
    StrBuf sb;
    sb_init(&sb, 8192);
    sb_printf(&sb, "{\n  \"font_size\": %d,\n  \"revision\": %d,\n  \"queue\": [",
              state->font_size, state->revision);
    for (int i = 0; i < state->queue_count; ++i) {
        QueueUser *u = &state->queue[i];
        if (i) sb_append(&sb, ",");
        sb_append(&sb, "\n    {\"id\":"); sb_json_string(&sb, u->id);
        sb_append(&sb, ",\"name\":"); sb_json_string(&sb, u->name);
        sb_append(&sb, ",\"source\":"); sb_json_string(&sb, u->source);
        sb_printf(&sb, ",\"joined_at\":%lld,\"priority\":%s}",
                  u->joined_at, u->priority ? "true" : "false");
    }
    sb_append(&sb, state->queue_count ? "\n  ],\n  \"pending_priority\": [" : "],\n  \"pending_priority\": [");
    for (int i = 0; i < state->pending_count; ++i) {
        PendingUser *u = &state->pending[i];
        if (i) sb_append(&sb, ",");
        sb_append(&sb, "\n    {\"id\":"); sb_json_string(&sb, u->id);
        sb_append(&sb, ",\"name\":"); sb_json_string(&sb, u->name);
        sb_printf(&sb, ",\"credits\":%d,\"updated_at\":%lld}", u->credits, u->updated_at);
    }
    sb_append(&sb, state->pending_count ? "\n  ]\n}\n" : "]\n}\n");
    _mkdir("data");
    if (sb.data) write_entire_file(state->state_path, sb.data, sb.len);
    sb_free(&sb);
}

static void changed_locked(AppState *state) {
    ++state->revision;
    save_locked(state);
}

void app_state_init(AppState *state, const char *state_path, long long room_id,
                    const char *keyword, const char *gift_name,
                    int font_size, int max_rows) {
    memset(state, 0, sizeof(*state));
    InitializeCriticalSection(&state->lock);
    state->room_id = room_id;
    state->font_size = font_size;
    state->max_rows = max_rows;
    copy_text(state->keyword, sizeof(state->keyword), keyword);
    copy_text(state->gift_name, sizeof(state->gift_name), gift_name);
    copy_text(state->state_path, sizeof(state->state_path), state_path);
    copy_text(state->live_state, sizeof(state->live_state), "starting");
    copy_text(state->live_message, sizeof(state->live_message), "程序正在启动");
}

void app_state_destroy(AppState *state) {
    DeleteCriticalSection(&state->lock);
}

void app_state_load(AppState *state) {
    size_t size = 0;
    char *json = read_entire_file(state->state_path, &size);
    if (!json || !size) { free(json); return; }
    EnterCriticalSection(&state->lock);
    state->font_size = (int)json_get_integer(json, "font_size", state->font_size);
    state->revision = (int)json_get_integer(json, "revision", 0);
    const char *array = json_find_value(json, "queue");
    if (array && *array == '[') {
        for (int i = 0; i < MAX_QUEUE_USERS; ++i) {
            const char *item = NULL; size_t len = 0;
            if (!json_array_element(array, i, &item, &len)) break;
            char *object = (char *)malloc(len + 1);
            if (!object) break;
            memcpy(object, item, len); object[len] = '\0';
            QueueUser user = {0};
            if (json_get_string(object, "id", user.id, sizeof(user.id)) &&
                json_get_string(object, "name", user.name, sizeof(user.name))) {
                if (!json_get_string(object, "source", user.source, sizeof(user.source)))
                    copy_text(user.source, sizeof(user.source), "bilibili");
                user.joined_at = json_get_integer(object, "joined_at", (long long)time(NULL));
                user.priority = json_get_boolean(object, "priority", false);
                state->queue[state->queue_count++] = user;
            }
            free(object);
        }
    }
    array = json_find_value(json, "pending_priority");
    if (array && *array == '[') {
        for (int i = 0; i < MAX_PENDING_USERS; ++i) {
            const char *item = NULL; size_t len = 0;
            if (!json_array_element(array, i, &item, &len)) break;
            char *object = (char *)malloc(len + 1);
            if (!object) break;
            memcpy(object, item, len); object[len] = '\0';
            PendingUser user = {0};
            if (json_get_string(object, "id", user.id, sizeof(user.id)) &&
                json_get_string(object, "name", user.name, sizeof(user.name))) {
                user.credits = (int)json_get_integer(object, "credits", 1);
                user.updated_at = json_get_integer(object, "updated_at", (long long)time(NULL));
                state->pending[state->pending_count++] = user;
            }
            free(object);
        }
    }
    LeaveCriticalSection(&state->lock);
    free(json);
}

void app_state_set_live(AppState *state, const char *status, const char *message) {
    EnterCriticalSection(&state->lock);
    copy_text(state->live_state, sizeof(state->live_state), status);
    copy_text(state->live_message, sizeof(state->live_message), message);
    LeaveCriticalSection(&state->lock);
    printf("[B站] %s\n", message);
}

char *app_state_snapshot(AppState *state, size_t *length) {
    StrBuf sb;
    sb_init(&sb, 16384);
    EnterCriticalSection(&state->lock);
    sb_append(&sb, "{\"type\":\"state\",\"queue\":[");
    for (int i = 0; i < state->queue_count; ++i) {
        QueueUser *u = &state->queue[i];
        if (i) sb_append(&sb, ",");
        sb_append(&sb, "{\"id\":"); sb_json_string(&sb, u->id);
        sb_append(&sb, ",\"name\":"); sb_json_string(&sb, u->name);
        sb_append(&sb, ",\"source\":"); sb_json_string(&sb, u->source);
        sb_printf(&sb, ",\"joined_at\":%lld,\"priority\":%s,\"position\":%d}",
                  u->joined_at, u->priority ? "true" : "false", i + 1);
    }
    sb_append(&sb, "],\"pending_priority\":[");
    for (int i = 0; i < state->pending_count; ++i) {
        PendingUser *u = &state->pending[i];
        if (i) sb_append(&sb, ",");
        sb_append(&sb, "{\"id\":"); sb_json_string(&sb, u->id);
        sb_append(&sb, ",\"name\":"); sb_json_string(&sb, u->name);
        sb_printf(&sb, ",\"credits\":%d,\"updated_at\":%lld}", u->credits, u->updated_at);
    }
    sb_printf(&sb, "],\"font_size\":%d,\"revision\":%d,\"room_id\":%lld,"
              "\"max_rows\":%d,\"keyword\":", state->font_size, state->revision,
              state->room_id, state->max_rows);
    sb_json_string(&sb, state->keyword);
    sb_append(&sb, ",\"priority_gifts\":["); sb_json_string(&sb, state->gift_name);
    sb_append(&sb, "],\"live_status\":{\"state\":"); sb_json_string(&sb, state->live_state);
    sb_append(&sb, ",\"message\":"); sb_json_string(&sb, state->live_message);
    sb_append(&sb, "}}");
    LeaveCriticalSection(&state->lock);
    if (length) *length = sb.len;
    return sb.data;
}

bool app_state_join(AppState *state, const char *id, const char *name,
                    const char *source, char *result, size_t result_size) {
    EnterCriticalSection(&state->lock);
    char resolved_id[MAX_USER_ID], resolved_name[MAX_USER_NAME];
    copy_text(resolved_id, sizeof(resolved_id), id);
    copy_text(resolved_name, sizeof(resolved_name), name);
    if (strncmp(id, "hash:", 5) == 0) {
        int matched = unique_pending_name_match(state, name);
        if (matched >= 0) {
            copy_text(resolved_id, sizeof(resolved_id), state->pending[matched].id);
            copy_text(resolved_name, sizeof(resolved_name), state->pending[matched].name);
        }
    }
    if (find_id(state, resolved_id) >= 0) {
        snprintf(result, result_size, "%s 已在队列中", name);
        LeaveCriticalSection(&state->lock);
        return false;
    }
    if (state->queue_count >= MAX_QUEUE_USERS) {
        copy_text(result, result_size, "队列已达到 512 人上限");
        LeaveCriticalSection(&state->lock);
        return false;
    }
    QueueUser user = {0};
    copy_text(user.id, sizeof(user.id), resolved_id);
    copy_text(user.name, sizeof(user.name), resolved_name);
    copy_text(user.source, sizeof(user.source), source);
    user.joined_at = (long long)time(NULL);
    int pending = find_pending(state, resolved_id);
    if (pending >= 0 && state->pending[pending].credits > 0) {
        user.priority = true;
        memmove(&state->queue[1], &state->queue[0], (size_t)state->queue_count * sizeof(QueueUser));
        state->queue[0] = user;
        ++state->queue_count;
        if (--state->pending[pending].credits <= 0) remove_pending(state, pending);
        snprintf(result, result_size, "%s 使用预存灯牌，插队到第 1 位", resolved_name);
    } else {
        state->queue[state->queue_count++] = user;
        snprintf(result, result_size, "%s 已加入第 %d 位", resolved_name, state->queue_count);
    }
    changed_locked(state);
    LeaveCriticalSection(&state->lock);
    printf("[队列] %s\n", result);
    return true;
}

bool app_state_gift(AppState *state, const char *id, const char *name,
                    char *result, size_t result_size) {
    EnterCriticalSection(&state->lock);
    int index = find_id(state, id);
    if (index < 0) {
        index = unique_queue_name_match(state, name);
        if (index >= 0) {
            copy_text(state->queue[index].id, sizeof(state->queue[index].id), id);
            copy_text(state->queue[index].name, sizeof(state->queue[index].name), name);
        }
    }
    if (index >= 0) {
        QueueUser user = state->queue[index];
        user.priority = true;
        if (index > 0) {
            memmove(&state->queue[1], &state->queue[0], (size_t)index * sizeof(QueueUser));
            state->queue[0] = user;
            snprintf(result, result_size, "%s 送出灯牌，已插队到第 1 位", name);
        } else {
            state->queue[0] = user;
            snprintf(result, result_size, "%s 已经在第 1 位", name);
        }
        changed_locked(state);
        LeaveCriticalSection(&state->lock);
        printf("[灯牌] %s\n", result);
        return true;
    }
    int pending = find_pending(state, id);
    if (pending < 0) {
        if (state->pending_count >= MAX_PENDING_USERS) {
            copy_text(result, result_size, "待用灯牌记录已满");
            LeaveCriticalSection(&state->lock);
            return false;
        }
        pending = state->pending_count++;
        memset(&state->pending[pending], 0, sizeof(PendingUser));
        copy_text(state->pending[pending].id, sizeof(state->pending[pending].id), id);
    }
    PendingUser *user = &state->pending[pending];
    copy_text(user->name, sizeof(user->name), name);
    ++user->credits;
    user->updated_at = (long long)time(NULL);
    snprintf(result, result_size, "已记录 %s 的灯牌，等待其发送“排队”", name);
    changed_locked(state);
    LeaveCriticalSection(&state->lock);
    printf("[灯牌] %s\n", result);
    return true;
}

static bool manual_add_locked(AppState *state, const char *name, int position,
                              char *result, size_t result_size) {
    if (!*name) { copy_text(result, result_size, "用户名不能为空"); return false; }
    int existing = find_name(state, name);
    QueueUser user = {0};
    if (existing >= 0) {
        user = state->queue[existing];
        remove_queue(state, existing);
    } else {
        if (state->queue_count >= MAX_QUEUE_USERS) {
            copy_text(result, result_size, "队列已满"); return false;
        }
        snprintf(user.id, sizeof(user.id), "manual:%lld:%d", (long long)time(NULL), state->revision + 1);
        copy_text(user.name, sizeof(user.name), name);
        copy_text(user.source, sizeof(user.source), "manual");
        user.joined_at = (long long)time(NULL);
    }
    if (position <= 0 || position > state->queue_count + 1) position = state->queue_count + 1;
    memmove(&state->queue[position], &state->queue[position - 1],
            (size_t)(state->queue_count - position + 1) * sizeof(QueueUser));
    state->queue[position - 1] = user;
    ++state->queue_count;
    snprintf(result, result_size, "已将 %s %s到第 %d 位", name, existing >= 0 ? "移动" : "加入", position);
    changed_locked(state);
    return true;
}

bool app_state_command(AppState *state, const char *command,
                       char *result, size_t result_size) {
    if (!command || !*command) { copy_text(result, result_size, "请输入命令"); return false; }
    if (_strnicmp(command, "test-queue-", 11) == 0) {
        const char *name = command + 11;
        char id[MAX_USER_ID]; snprintf(id, sizeof(id), "test:%s", name);
        return app_state_join(state, id, name, "test", result, result_size);
    }
    if (_strnicmp(command, "test-gift-", 10) == 0) {
        const char *name = command + 10;
        char id[MAX_USER_ID]; snprintf(id, sizeof(id), "test:%s", name);
        return app_state_gift(state, id, name, result, result_size);
    }
    EnterCriticalSection(&state->lock);
    bool changed = false;
    if (_strnicmp(command, "del-", 4) == 0) {
        int position = atoi(command + 4);
        if (position < 1 || position > state->queue_count) {
            snprintf(result, result_size, "没有第 %d 位用户", position);
        } else {
            char name[MAX_USER_NAME]; copy_text(name, sizeof(name), state->queue[position - 1].name);
            remove_queue(state, position - 1);
            snprintf(result, result_size, "已删除第 %d 位：%s", position, name);
            changed_locked(state); changed = true;
        }
    } else if (_strnicmp(command, "font-", 5) == 0) {
        int size = atoi(command + 5);
        if (size < 12 || size > 160) copy_text(result, result_size, "字体大小须在 12 到 160 之间");
        else {
            state->font_size = size;
            snprintf(result, result_size, "字体大小已设为 %dpx", size);
            changed_locked(state); changed = true;
        }
    } else if (_stricmp(command, "clear") == 0) {
        int count = state->queue_count;
        state->queue_count = 0;
        snprintf(result, result_size, "已清空 %d 位用户", count);
        if (count) { changed_locked(state); changed = true; }
    } else if (_stricmp(command, "list") == 0) {
        if (!state->queue_count) copy_text(result, result_size, "当前队列为空");
        else {
            StrBuf sb; sb_init(&sb, 512);
            for (int i = 0; i < state->queue_count; ++i) {
                if (i) sb_append(&sb, " | ");
                sb_printf(&sb, "%d -> %s", i + 1, state->queue[i].name);
            }
            copy_text(result, result_size, sb.data); sb_free(&sb);
        }
    } else if (_stricmp(command, "help") == 0) {
        copy_text(result, result_size, "命令：del-3 / add-你好 / add-你好-3 / font-36 / clear / list / test-queue-名字 / test-gift-名字");
    } else if (_strnicmp(command, "add-", 4) == 0) {
        char name[MAX_USER_NAME]; copy_text(name, sizeof(name), command + 4);
        int position = 0;
        char *dash = strrchr(name, '-');
        if (dash && dash[1]) {
            bool digits = true;
            for (char *p = dash + 1; *p; ++p) if (*p < '0' || *p > '9') digits = false;
            if (digits) { position = atoi(dash + 1); *dash = '\0'; }
        }
        changed = manual_add_locked(state, name, position, result, result_size);
    } else copy_text(result, result_size, "未知命令，输入 help 查看帮助");
    LeaveCriticalSection(&state->lock);
    if (changed) printf("[命令] %s\n", result);
    return changed || _stricmp(command, "list") == 0 || _stricmp(command, "help") == 0;
}

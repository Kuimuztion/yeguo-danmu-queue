#ifndef BYCUTE_QUEUE_H
#define BYCUTE_QUEUE_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_QUEUE_USERS 512
#define MAX_PENDING_USERS 512
#define MAX_USER_ID 96
#define MAX_USER_NAME 256

typedef struct {
    char id[MAX_USER_ID];
    char name[MAX_USER_NAME];
    char source[24];
    long long joined_at;
    bool priority;
} QueueUser;

typedef struct {
    char id[MAX_USER_ID];
    char name[MAX_USER_NAME];
    int credits;
    long long updated_at;
} PendingUser;

typedef struct {
    CRITICAL_SECTION lock;
    QueueUser queue[MAX_QUEUE_USERS];
    int queue_count;
    PendingUser pending[MAX_PENDING_USERS];
    int pending_count;
    int font_size;
    int max_rows;
    int revision;
    long long room_id;
    char keyword[64];
    char gift_name[128];
    char live_state[32];
    char live_message[256];
    char auth_cookie[4096];
    char auth_buvid[256];
    long long auth_uid;
    char state_path[MAX_PATH];
} AppState;

void app_state_init(AppState *state, const char *state_path, long long room_id,
                    const char *keyword, const char *gift_name,
                    int font_size, int max_rows);
void app_state_destroy(AppState *state);
void app_state_load(AppState *state);
void app_state_set_live(AppState *state, const char *status, const char *message);
char *app_state_snapshot(AppState *state, size_t *length);

bool app_state_join(AppState *state, const char *id, const char *name,
                    const char *source, char *result, size_t result_size);
bool app_state_gift(AppState *state, const char *id, const char *name,
                    char *result, size_t result_size);
bool app_state_command(AppState *state, const char *command,
                       char *result, size_t result_size);

#endif

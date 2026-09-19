#ifndef BYCUTE_WEB_SERVER_H
#define BYCUTE_WEB_SERVER_H

#include "queue.h"
#include <windows.h>

typedef struct {
    AppState *state;
    int port;
} WebServerArgs;

DWORD WINAPI web_server_thread(LPVOID parameter);

#endif


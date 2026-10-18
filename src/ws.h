/* ws.h — local WebSocket server for browser extensions (NeatWebSocketServer) */
#ifndef RENEAT_WS_H
#define RENEAT_WS_H
#include "common.h"

/* job posted to the UI thread when an extension pushes a download */
typedef struct {
    char url[2048];
    char referer[2048];
    char cookies[2048];
    char useragent[256];
    char filename[512];
} WsJob;

int  ws_start(int port);        /* listener thread; 0 ok */
void ws_stop(void);
int  ws_port(void);

/* implemented in ui.c: called from the WS client thread, must marshal to UI thread */
void ui_submit_ws_job(WsJob *job);

#endif

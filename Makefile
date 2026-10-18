# reneat — Neat Download Manager clone in native Win32 C
# Default (and only) target: reneat.exe

CC      = gcc
CFLAGS  = -Os -Wall -Wextra -Wno-unused-parameter -std=c11 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -Isrc -MMD -MP
LDFLAGS = -municode -mwindows -Wl,--subsystem,windows
LIBS    = -lws2_32 -lsecur32 -lbcrypt -lcomctl32 -luxtheme -ldwmapi -lgdi32 -luser32 -lkernel32 -lshell32 -lole32 -lcomdlg32 -lshlwapi

SRC = src/main.c src/cli.c src/ui.c src/prog.c src/dlg.c src/settings.c src/lang.c src/ws.c src/net.c src/tls.c src/http.c src/engine.c src/util.c src/store.c
OBJ = $(SRC:.c=.o) res/reneat_res.o

all: reneat.exe

RES_ICONS = $(wildcard res/extracted/*.ico)

res/reneat_res.o: res/reneat.rc $(RES_ICONS)
	windres -DUNICODE -D_UNICODE res/reneat.rc -o res/reneat_res.o

reneat.exe: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(SRC:.c=.d)

clean:
	-del /Q src\*.o src\*.d res\*.o reneat.exe 2>NUL & exit 0

.PHONY: all clean

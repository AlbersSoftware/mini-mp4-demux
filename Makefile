# =============================================================================
#  MP4 Sequential Demuxer -- cross-platform Makefile
#  Works with: GnuWin32 make, MSYS2 make, and Unix/macOS make
# =============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -O2 -std=c99
LDFLAGS = -lm
SRCS    = main.c mp4_demux.c
HDRS    = mp4_demux.h minimp3.h

# ---- Platform detection (Windows sets OS=Windows_NT automatically) ----------
ifeq ($(OS),Windows_NT)
    TARGET  = mp4demux.exe
    RM      = del /Q
    DEVNULL = nul
else
    TARGET  = mp4demux
    RM      = rm -f
    DEVNULL = /dev/null
endif

.PHONY: all clean fetch-minimp3 test

# ---- Default target ---------------------------------------------------------
all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo Build OK: $@

# ---- Fetch minimp3 (single-header, MIT licensed) ----------------------------
#
#  minimp3.h is a public-domain single-header MP3 decoder by lieff.
#  Download it once; it is not included in the repo to keep size down.
#
minimp3.h:
	curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h -o minimp3.h
	@echo minimp3.h downloaded.

fetch-minimp3: minimp3.h

# ---- Generate a FastStart test file (requires ffmpeg in PATH) ---------------
#
#  Creates a 5-second 320x240 colour-bar video with a 440 Hz sine tone.
#  Audio is MP3 so minimp3 can decode it directly.
#  -movflags faststart puts moov before mdat (required by this demuxer).
#
#  The ffmpeg command is written as a single line to avoid shell-continuation
#  issues on Windows cmd.exe.
#
test.mp4:
	ffmpeg -y -f lavfi -i "testsrc=duration=5:size=320x240:rate=24" -f lavfi -i "sine=frequency=440:duration=5" -c:v libx264 -preset ultrafast -c:a libmp3lame -b:a 128k -movflags faststart test.mp4

# ---- Run demuxer on the test file -------------------------------------------
test: $(TARGET) test.mp4
	$(TARGET) test.mp4

# ---- Clean ------------------------------------------------------------------
clean:
	-$(RM) $(TARGET) test.mp4 2>$(DEVNULL)

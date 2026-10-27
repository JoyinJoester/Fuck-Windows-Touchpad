# Fuck-Windows-Touchpad — MinGW build
#
# Requires: MinGW-w64 g++ (C:\msys64\mingw64\bin) + mingw32-make
#
#   make          build build/Fuck-Windows-Touchpad.exe
#   make dist     build + a clean folder ready to zip
#   make run      build and launch
#   make clean    remove build/
#
# The binary is a GUI-subsystem executable (-mwindows): double-clicking it opens
# the window with no console. Everything it needs is linked statically apart
# from the Win32/GDI+ system DLLs, so the output is a single portable file.

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Isrc -DUNICODE -D_UNICODE \
            -finput-charset=UTF-8
LDFLAGS  := -mwindows -municode
LDLIBS   := -lhid -lsetupapi -luser32 -ladvapi32 -ldwmapi -lshell32 \
            -lgdiplus -lole32

SRC := src/main.cpp \
       src/Touchpad.cpp \
       src/GestureEngine.cpp \
       src/RawInputSource.cpp \
       src/Preflight.cpp \
       src/WindowManager.cpp \
       src/Config.cpp \
       src/Action.cpp \
       src/EventLog.cpp \
       src/UiWindow.cpp

OUT     := build/Fuck-Windows-Touchpad.exe
DIST    := dist/Fuck-Windows-Touchpad

.PHONY: all clean run dist

all: $(OUT)

$(OUT): $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT) $(LDFLAGS) $(LDLIBS)
	@echo "built $(OUT)"

run: $(OUT)
	./$(OUT)

# A folder that can be zipped and handed to someone: the exe plus a commented
# default config. No runtime, no installer, no registry writes.
dist: $(OUT)
	@mkdir -p $(DIST)
	@cp $(OUT) $(DIST)/
	@cp tp_gestures.ini $(DIST)/ 2>/dev/null || true
	@echo "packaged -> $(DIST)"

clean:
	rm -rf build dist

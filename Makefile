# Cross-compiles the layer and the gamepad provider with mingw-w64 (brew install mingw-w64).
CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall
OUT = build

all: $(OUT)/d3d12.dll $(OUT)/fh2pad.dll

tools: $(OUT)/fh2test.exe $(OUT)/padtest.exe $(OUT)/dxbcdis.exe

$(OUT):
	mkdir -p $(OUT)

$(OUT)/d3d12.dll: src/d3d12/d3d12.c src/d3d12/d3d12.def src/d3d12/present.inc | $(OUT)
	$(CC) $(CFLAGS) -shared -static-libgcc -o $@ src/d3d12/d3d12.c src/d3d12/d3d12.def -ldxguid -luuid

$(OUT)/fh2pad.dll: src/gamepad/gamepad.c src/gamepad/gamepad.def | $(OUT)
	$(CC) $(CFLAGS) -shared -static-libgcc -o $@ $^ -luuid

$(OUT)/fh2test.exe: tools/fh2test.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/padtest.exe: tools/padtest.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< -lruntimeobject

$(OUT)/dxbcdis.exe: tools/dxbcdis.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all tools clean

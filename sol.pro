QT += core gui dbus
CONFIG += c++17 release
TARGET = sol
SOURCES += src/main.cpp src/cli_ui.cpp src/audiofeedback.cpp src/backend.cpp src/desktop.cpp src/formatter.cpp src/hudwindow.cpp src/speechengine.cpp
HEADERS += src/cli_ui.h src/audiofeedback.h src/backend.h src/desktop.h src/formatter.h src/hudwindow.h src/speechengine.h
RESOURCES += src/integration.qrc src/speech.qrc

# Size-optimized release flags: -Os + LTO + hidden visibility + no C++ exception unwind tables.
QMAKE_CXXFLAGS_RELEASE -= -O2 -O3
QMAKE_CXXFLAGS_RELEASE += -Os -flto=auto -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -fmerge-all-constants
QMAKE_LFLAGS_RELEASE += -flto=auto -Wl,--gc-sections -Wl,--as-needed -Wl,-O1
DEFINES += QT_NO_DEBUG_OUTPUT QT_NO_EXCEPTIONS

exists(/usr/include/LayerShellQt/Window) {
    exists(/usr/lib/libLayerShellQtInterface.so)|exists(/usr/lib64/libLayerShellQtInterface.so)|exists(/usr/lib/x86_64-linux-gnu/libLayerShellQtInterface.so)|exists(/usr/lib/aarch64-linux-gnu/libLayerShellQtInterface.so) {
        LIBS += -lLayerShellQtInterface
        DEFINES += SOL_LAYER_SHELL
    }
}

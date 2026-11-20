QT += core gui dbus testlib
CONFIG += testcase console c++17
SOURCES += backend_tests.cpp ../src/backend.cpp ../src/audiofeedback.cpp ../src/cli_ui.cpp ../src/desktop.cpp ../src/formatter.cpp ../src/hudwindow.cpp ../src/speechengine.cpp
HEADERS += ../src/backend.h ../src/audiofeedback.h ../src/cli_ui.h ../src/desktop.h ../src/formatter.h ../src/hudwindow.h ../src/speechengine.h
RESOURCES += ../src/integration.qrc ../src/speech.qrc
TARGET = backend-tests

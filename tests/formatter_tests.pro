QT += core testlib
QT -= gui
CONFIG += testcase console c++17
SOURCES += formatter_tests.cpp ../src/formatter.cpp
HEADERS += ../src/formatter.h
TARGET = formatter-tests

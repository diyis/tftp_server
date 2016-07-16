TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += main.c \
    tftp.c \
    client.c

HEADERS += \
    tftp.h

DISTFILES += \
    makefile

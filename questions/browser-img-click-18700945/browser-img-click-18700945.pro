QT += gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets
DEFINES += \
  QT_DEPRECATED_WARNINGS \
  QT_DISABLE_DEPRECATED_BEFORE=0x060000 \
  QT_RESTRICTED_CAST_FROM_ASCII
SOURCES += main.cpp
exists(../../tooling/tooling.pri): include(../../tooling/tooling.pri)

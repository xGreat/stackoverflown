QT = charts
CONFIG += c++11
DEFINES += \
  QT_DEPRECATED_WARNINGS \
  QT_DISABLE_DEPRECATED_BEFORE=0x060000 \
  QT_RESTRICTED_CAST_FROM_ASCII
SOURCES = \
    main.c \
    qc_binding.cpp
HEADERS = \
    qc_binding.h

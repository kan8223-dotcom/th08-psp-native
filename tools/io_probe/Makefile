TARGET = TH08IOPROBE
OBJS = main.o
BUILD_PRX = 1
PSP_FW_VERSION = 660
CFLAGS = -O2 -G0 -Wall -Wextra -D_PSP_FW_VERSION=660
LIBDIR =
LDFLAGS =
LIBS = -lpspctrl -lpspdebug -lpspdisplay -lpspgu -lpspge -lpsppower -lpspaudio
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = TH08 IO PROBE
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# Preserve the installed r238 binary's historical bootstrap/error-screen label.
# The original incremental build retained video.o from r236. Other translation
# units use the r238 build ID. This is identity preservation, not a game change.
psp/video.o: CXXFLAGS += -UTH08_PSP_BUILD_ID -DTH08_PSP_BUILD_ID=\"r236-nolog-20260909_142801\"

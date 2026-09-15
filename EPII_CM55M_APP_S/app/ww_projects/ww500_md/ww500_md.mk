override SCENARIO_APP_SUPPORT_LIST := $(APP_TYPE)

##
# Some of the longer TFLM source filenames (e.g.
# recording_single_arena_buffer_allocator.cc), once nested under the normal
# obj_<board>/<toolchain>_<board>_<pkg>/library/.../ output tree, push the
# full .o/.d/.su path past Windows' 260-character MAX_PATH limit when the
# project is checked out under a long path - causing intermittent-looking
# "cannot open ...su for writing: No such file or directory" compiler
# errors that depend on exactly how long your checkout path is.
#
# If D:\hxbuild exists (or can be created), relocate the object tree there
# via the build's own OUT_DIR_ROOT override (see `make help`) - this only
# changes where .o/.d/.su files are written, not the source layout.
#
# Safe on other machines/checkouts: if D: isn't available the mkdir below
# fails silently and OUT_DIR_ROOT is left unset, so the build falls back to
# today's normal in-project object tree. Override from the command line
# with a non-empty path if you want a different location, e.g.
# make OUT_DIR_ROOT=E:/build (that bypasses this block entirely, since it
# only acts when OUT_DIR_ROOT is empty).
#
# Uses "override" because some IDEs (e.g. Eclipse CDT) pass every build
# variable explicitly on the command line, including blank ones - a plain
# assignment here cannot replace a command-line-set variable, even an
# empty one, without it.
##
# Windows only: on Linux/macOS "D:/hxbuild" is a *relative* path, so the mkdir
# below creates a directory literally called "D:" and OUT_DIR_ROOT then puts a
# colon into every object path - which make parses as a rule separator
# ("target pattern contains no '%'"). MAX_PATH is a Windows problem, so the
# workaround stays on Windows.
ifeq "$(HOST_OS)" "Windows"
ifeq ($(strip $(OUT_DIR_ROOT)),)
WW500_BUILD_ROOT := D:/hxbuild
WW500_BUILD_ROOT_NATIVE := $(subst /,$(PS),$(WW500_BUILD_ROOT))
$(shell $(IFNOTEXISTDIR) $(WW500_BUILD_ROOT_NATIVE) $(ENDIFNOTEXISTDIR) $(MKD) $(WW500_BUILD_ROOT_NATIVE) 2> $(NULL))
ifneq ($(wildcard $(WW500_BUILD_ROOT)),)
override OUT_DIR_ROOT := $(WW500_BUILD_ROOT)
endif
endif
endif	# HOST_OS == Windows

##
# Skip recompiling TensorFlow Lite Micro and CMSIS-NN from source on every
# (clean) build. Both are vendored library code that never changes here, and
# together they're the vast majority of a full build's time (~150 TFLM .cc
# files, plus all of CMSIS-NN's Source/**).
#
# The SDK already supports this: options/prebuilt_force.mk has
# INFERENCE_FORCE_PREBUILT / CMSIS_NN_LIB_FORCE_PREBUILT flags (default n)
# that, when y, make each library's own .mk rule
# (library/inference/.../*.mk, library/cmsis_nn/.../*.mk) skip compiling its
# sources entirely and just copy the already-built archive from
# prebuilt_libs/$(TOOLCHAIN)/ into $(OUT_DIR) instead. A *normal* (non-forced)
# build already keeps that archive current - it re-copies its freshly-built
# .a back into prebuilt_libs/ every time - so as long as one normal build has
# happened since the last genuine TFLM/CMSIS-NN source change, the prebuilt
# copy is up to date. It's also unaffected by the D:\hxbuild fix above:
# PREBUILT_LIB is a plain in-project-relative path, not under OUT_DIR_ROOT.
#
# Routed through our own WW500_FAST_LIBS switch (default y) rather than
# driving the two vendor flags directly from the command line: doing that
# with "override" (as OUT_DIR_ROOT does above) would remove your own escape
# hatch for forcing a real rebuild, since override beats every source
# including the command line. This way, to actually rebuild one of those
# libraries from source (e.g. after an SDK upgrade), use:
#   make WW500_FAST_LIBS=n
##
ifeq ($(strip $(WW500_FAST_LIBS)),)
override WW500_FAST_LIBS := y
endif
ifeq ($(strip $(WW500_FAST_LIBS)),y)
override INFERENCE_FORCE_PREBUILT := y
override CMSIS_NN_LIB_FORCE_PREBUILT := y
endif

# Get git info
# Use $(NULL) (options/scripts.mk: /dev/null on Linux/WSL, NUL on Windows) rather
# than a literal 2>NUL - the latter creates a stray file called NUL under Linux/WSL
# (there is no such special device name there), which then breaks Windows-native
# git.exe (NUL is a reserved device name on Windows, so it can't open a real file
# with that name) if the working copy is ever also touched by git on Windows.
GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>$(NULL))
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>$(NULL))
GIT_DIRTY  := $(shell git diff --quiet 2>$(NULL) || echo -dirty)
ifeq ($(GIT_BRANCH),)
GIT_BRANCH := nogit
endif

ifeq ($(GIT_COMMIT),)
GIT_COMMIT := nogit
endif

ifeq ($(GIT_DIRTY),)
GIT_DIRTY :=
endif
$(info Git information: GIT_BRANCH='${GIT_BRANCH}' GIT_COMMIT='${GIT_COMMIT}' GIT_DIRTY='${GIT_DIRTY}') 

APPL_DEFINES += \
    -DGIT_BRANCH=\"$(GIT_BRANCH)\" \
    -DGIT_COMMIT=\"$(GIT_COMMIT)\" \
    -DGIT_DIRTY=\"$(GIT_DIRTY)\"

# Force rebuild of the main .c file which has __TIME__ and __DATE__ so the latest time and date is printed on every build.
# Define a place where the object file is placed:
OBJECT_DESTINATION = obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65
.PHONY: force_rebuild_main
force_rebuild_main:
	@echo Forcing rebuild
#obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/app/ww_projects/$(APP_TYPE)/$(APP_TYPE).o: force_rebuild_main
$(OBJECT_DESTINATION)/app/ww_projects/$(APP_TYPE)/$(APP_TYPE).o: force_rebuild_main
all: force_rebuild_main

##
# 'make clean' (defined in options/rules.mk) only removes $(OUT_DIR), which
# on Windows is now under D:\hxbuild per the fix above - it never touches
# this project's old in-project object folder. Without this, that folder is
# orphaned rather than cleaned, and if OUT_DIR_ROOT ever falls back to empty
# (D: unavailable, or an explicit override) a plain 'make' would resume
# building incrementally on top of whatever stale .o files are sitting in
# it - possibly compiled with different flags/camera variant - without
# anyone noticing. Reuses OBJECT_DESTINATION (above) rather than
# reconstructing the path from BOARD_INFO/BUILD_INFO, since those aren't
# set yet at any point during this file's own execution.
##
.PHONY: clean_legacy_obj
clean_legacy_obj:
	-$(IFEXISTDIR) $(subst /,$(PS),$(OBJECT_DESTINATION)) $(ENDIFEXISTDIR) $(RMD) $(subst /,$(PS),$(OBJECT_DESTINATION))

clean: clean_legacy_obj

##
# Stage the built .elf for image generation (see
# _Documentation/building_firmware.md, and _Documentation/Compile_and_flash.md
# section 3b for the DPD/deep-power-down bootloader we actually ship), replacing
# the manual "copy the .elf file" step previously done by hand before running
# we2_local_image_gen (see the gen_image target in image_gen.mk for that step).
##
WW500_STAGE_ELF_DIR = $(EPII_ROOT)/../we2_image_gen_local_dpd/input_case1_secboot
WW500_STAGE_ELF_DIR_NATIVE = $(subst /,$(PS),$(WW500_STAGE_ELF_DIR))
$(shell $(IFNOTEXISTDIR) $(WW500_STAGE_ELF_DIR_NATIVE) $(ENDIFNOTEXISTDIR) $(MKD) $(WW500_STAGE_ELF_DIR_NATIVE) 2> $(NULL))

# APPL_FULL_NAME/ELF_FILENAME aren't defined yet at this point in the include
# chain (options.mk sets them later, after this file is included via
# app.mk), so .SECONDEXPANSION is needed to defer this prerequisite's
# expansion until the whole makefile has been read.
.PHONY: stage_elf
.SECONDEXPANSION:
stage_elf: $$(APPL_FULL_NAME).$$(ELF_FILENAME)
	@$(ECHO) "Staging ELF for image generation: " $<
	$(Q)$(CP) $(subst /,$(PS),$<) $(WW500_STAGE_ELF_DIR_NATIVE)$(PS)

all: stage_elf

# The APPL_DEFINES line below must match this line in ww.mk:
# APP_TYPE = ww500_md
APPL_DEFINES += -DWW500_MD

#APPL_DEFINES += -DIP_xdma
#APPL_DEFINES += -DEVT_DATAPATH

#APPL_DEFINES += -DEVT_CM55MTIMER -DEVT_CM55MMB
APPL_DEFINES += -DDBG_MORE

#EVENTHANDLER_SUPPORT = event_handler
#EVENTHANDLER_SUPPORT_LIST += evt_datapath

##
# library support feature
# Add new library here
# The source code should be loacted in ~\library\{lib_name}\
##
#LIB_SEL = pwrmgmt sensordp tflmtag2209_u55tag2205 spi_ptl spi_eeprom i2c_comm #hxevent
LIB_SEL = pwrmgmt sensordp tflmtag2412_u55tag2411 spi_ptl spi_eeprom i2c_comm #hxevent

# Add a compiler switch if we select the later TFLM library:
ifeq ($(filter tflmtag2412_u55tag2411,$(LIB_SEL)),tflmtag2412_u55tag2411)
    APPL_DEFINES += -DTFLM_2412
endif


##
# middleware support feature
# Add new middleware here
# The source code should be loacted in ~\middleware\{mid_name}\
##
MID_SEL = fatfs
#MID_SEL =
FATFS_PORT_LIST = mmc_spi
CMSIS_DRIVERS_LIST = SPI

#override OS_SEL := freertos
override OS_SEL := freertos_10_5_1
override OS_HAL := n
override MPU := n
override TRUSTZONE := y
override TRUSTZONE_TYPE := security
override TRUSTZONE_FW_TYPE := 1
override CIS_SEL := HM_COMMON
override EPII_USECASE_SEL := drv_onecore_cm55m_s

CIS_SUPPORT_INAPP = cis_sensor
# Default camera variant for a plain `make`. Both variants ship, and CI builds
# both, so this only decides what you get without an override:
#   make CIS_SUPPORT_INAPP_MODEL=cis_hm0360
# RP3 is the default because it is the day/colour camera most development and
# bench work targets. The HM0360 remains present for motion detection either
# way (USE_HM0360_MD below).
#CIS_SUPPORT_INAPP_MODEL = cis_hm0360
# OV5647 for RP v1 camera
#CIS_SUPPORT_INAPP_MODEL = cis_ov5647
# IMX219 for RP v2 camera
#CIS_SUPPORT_INAPP_MODEL = cis_imx219
#CIS_SUPPORT_INAPP_MODEL = cis_imx477
# IMX708 for RP v3 camera (main camera; HM0360 remains for motion detection via USE_HM0360_MD)
CIS_SUPPORT_INAPP_MODEL = cis_imx708

# CGP added to indicate HM0360 is used:

ifeq ($(CIS_SUPPORT_INAPP_MODEL), cis_hm0360)
$(info Using HM0360)
APPL_DEFINES += -DUSE_HM0360
else ifeq ($(CIS_SUPPORT_INAPP_MODEL), cis_imx219)
$(info Using IMX219)
APPL_DEFINES += -DCIS_IMX
APPL_DEFINES += -DUSE_RP2
APPL_DEFINES += -DUSE_HM0360_MD
else ifeq ($(CIS_SUPPORT_INAPP_MODEL), cis_imx477)
$(info Using IMX477)
APPL_DEFINES += -DCIS_IMX
APPL_DEFINES += -DUSE_HM0360_MD
else ifeq ($(CIS_SUPPORT_INAPP_MODEL), cis_imx708)
$(info Using IMX708)
APPL_DEFINES += -DCIS_IMX
APPL_DEFINES += -DUSE_RP3
APPL_DEFINES += -DUSE_HM0360_MD
endif

$(info In ww500_md.mk TOOLCHAIN='${TOOLCHAIN}', SCENARIO_APP_ROOT='${SCENARIO_APP_ROOT}',  APP_TYPE='${APP_TYPE}') 

ifeq ($(strip $(TOOLCHAIN)), arm)
# CGP change: to have the name of the linker script the same as the APP_TYPE
#override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/allon_sensor_tflm.sct
# override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/$(APP_TYPE).sct
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/ww500_md.sct
else#TOOLChain
#override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/allon_sensor_tflm.ld
# override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/$(APP_TYPE).ld
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/ww500_md.ld
endif
	
$(info In ww500_md.mk LINKER_SCRIPT_FILE='${LINKER_SCRIPT_FILE}')
##
# Add new external device here
# The source code should be located in ~\external\{device_name}\
##
#EXT_DEV_LIST += 

$(info In ww500_md.mk CIS_SUPPORT_INAPP_MODEL='${CIS_SUPPORT_INAPP_MODEL}' SCENARIO_APP_INCDIR='${SCENARIO_APP_INCDIR}')
# CGP this should have printed useful information, but does not:
# $(info USE_SPECS='${USE_SPECS}' USE_NANO='${USE_NANO}')

# Post-processing (gen_image, device_image): turn the compiled .elf into a
# flashable image. Included here, after CIS_SUPPORT_INAPP_MODEL is set above,
# because that block needs it - see mk/image_gen.mk for why it lives in mk/
# rather than directly in this folder (ww.mk auto-includes every *.mk file
# it finds directly here, which would otherwise include it a second time,
# too early).
include $(SCENARIO_APP_ROOT)/$(APP_TYPE)/mk/image_gen.mk


##
# Post-processing: turn the compiled .elf into a flashable/device-usable
# firmware image (gen_image, device_image). Split out of ww500_md.mk to keep
# that file to just the build-path fix (D:\hxbuild) and the .elf staging
# step (stage_elf).
#
# Lives in mk/, not directly in ww500_md/, so ww.mk's own project-loader
# (which auto-includes every *.mk file directly in ww500_md/, non-recursive)
# does not also pick this up - it must only be included the one, deliberate
# way below, from the end of ww500_md.mk after CIS_SUPPORT_INAPP_MODEL is
# set (device_image needs it) - not from where stage_elf/gen_image used to
# live, which was before that point. Also depends on the 'stage_elf' target
# and on EPII_ROOT/HOST_OS/PS/CP/ECHO/Q (from options/scripts.mk), both
# already available by the time ww500_md.mk includes this file.
##

##
# Run the image-generation tool (see _Documentation/building_firmware.md) on
# the staged .elf to produce output_case1_sec_wlcsp/output.img, replacing the
# manual "run we2_local_image_gen ..." step. Uses the RC24M/DPD bootloader
# profile, which is what deployed WW500 units actually use.
#
# Always runs (.PHONY): the secure-boot signer embeds fresh random bytes on
# every run (see building_firmware.md - images are not byte-reproducible
# anyway), so there is no "up to date" output to skip.
#
# On Windows this needs these binaries present in we2_image_gen_local_dpd/
# (paths relative to that folder):
#   we2_local_image_gen.exe
#   arm_none_eabi/arm-none-eabi-objcopy.exe
#   arm_none_eabi/arm-none-eabi-objdump.exe
#   secureboot_tool/generate_secureboot_certificates.exe   (shelled out to by
#                                                            we2_local_image_gen.exe)
# All of these are *.exe and therefore gitignored (never committed), so on a
# fresh checkout they must be copied in by hand once, from another working
# we2_image_gen_local_dpd folder or from Himax's SDK download. Without them,
# gen_image fails with a cryptic Python
# "FileNotFoundError: [WinError 2] The system cannot find the file specified"
# traceback from inside we2_local_image_gen.exe rather than a clear message -
# the check below exists so a missing file is reported plainly instead.
##
WW500_IMAGE_GEN_DIR = $(EPII_ROOT)/../we2_image_gen_local_dpd
WW500_IMAGE_GEN_DIR_NATIVE = $(subst /,$(PS),$(WW500_IMAGE_GEN_DIR))
WW500_IMAGE_GEN_JSON = project_case1_blp_wlcsp_rc24m.json
ifeq ($(HOST_OS),Windows)
WW500_IMAGE_GEN_EXE = we2_local_image_gen.exe
WW500_IMAGE_GEN_REQUIRED_EXES = \
	$(WW500_IMAGE_GEN_DIR)/we2_local_image_gen.exe \
	$(WW500_IMAGE_GEN_DIR)/arm_none_eabi/arm-none-eabi-objcopy.exe \
	$(WW500_IMAGE_GEN_DIR)/arm_none_eabi/arm-none-eabi-objdump.exe \
	$(WW500_IMAGE_GEN_DIR)/secureboot_tool/generate_secureboot_certificates.exe
WW500_IMAGE_GEN_MISSING_EXES = $(filter-out $(wildcard $(WW500_IMAGE_GEN_REQUIRED_EXES)),$(WW500_IMAGE_GEN_REQUIRED_EXES))
else
WW500_IMAGE_GEN_EXE = ./we2_local_image_gen
endif

.PHONY: gen_image
gen_image: stage_elf
ifneq ($(strip $(WW500_IMAGE_GEN_MISSING_EXES)),)
	$(error gen_image is missing required Windows tool(s) - see the comment above this target in mk/image_gen.mk for what to copy in and from where: $(WW500_IMAGE_GEN_MISSING_EXES))
endif
	@$(ECHO) "Generating firmware image: " $(WW500_IMAGE_GEN_JSON)
	$(Q)cd $(WW500_IMAGE_GEN_DIR_NATIVE) && $(WW500_IMAGE_GEN_EXE) $(WW500_IMAGE_GEN_JSON)

all: gen_image

##
# Also produce a short, 8.3-compatible copy of the generated image, matching
# the naming convention the website's "Prepare SD Card" generator uses (see
# _Documentation/firmware_update_and_recovery.md and
# EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/README.TXT) - the
# on-device FatFs only supports 8.3 short names, so this is what actually
# lets a locally-built image be dropped into the SD card's MANIFEST folder,
# used with the console 'firmware <file>' command, or burned via X-Modem
# exactly like a website-generated one. output.img itself is left alone
# (untouched, same name) since the existing burn/XMODEM docs and scripts all
# reference it by that fixed name - this is an additional copy.
#
# Format: VYMDDHMM.IMG (from local build-machine time, matching __DATE__/
# __TIME__ - see the note below)
#   V  = camera variant: R (RP3) or H (HM0360)
#   Y  = last digit of the year
#   M  = month: 1-9, then A=Oct, B=Nov, C=Dec
#   DD = day, zero-padded
#   H  = hour: 0-9, then A-N = 10-23
#   MM = minute, zero-padded
# Example from the doc: R6707N35.IMG = RP3, 2026, July, day 07, hour 23 (N),
# minute 35 - hand-checked against that example below.
#
# Uses local time, not UTC: __DATE__/__TIME__ (the C standard macros the
# firmware embeds and shows on the 'ver' boot banner - see
# firmware_update_and_recovery.md) have no timezone conversion, so they're
# always the build machine's local clock. Using local time here too means
# this filename and the boot banner always agree on build time, rather than
# differing by the local UTC offset (e.g. 12-13h for NZ).
#
# Needs CIS_SUPPORT_INAPP_MODEL, which is why this file is included from
# ww500_md.mk after that is set.
##
ifeq ($(CIS_SUPPORT_INAPP_MODEL),cis_hm0360)
WW500_IMG_VARIANT_LETTER = H
else
WW500_IMG_VARIANT_LETTER = R
endif

# Raw local date parts as one shell call: year-last-digit, day(2-digit),
# minute(2-digit), month(1-12, unpadded), hour+1(1-24, unpadded - the +1 is
# so it lines up with $(word)'s 1-based indexing below).
ifeq ($(HOST_OS),Windows)
WW500_IMG_DATE_RAW := $(shell powershell -NoProfile -Command "$$d=Get-Date; Write-Output ('{0} {1:00} {2:00} {3} {4}' -f ($$d.Year % 10), $$d.Day, $$d.Minute, $$d.Month, ($$d.Hour + 1))")
else
# Best-effort / not verified against a real Linux build - GNU date's %-m/%-H
# (no leading zero) may not exist on non-GNU (e.g. macOS/BSD) date.
WW500_IMG_DATE_RAW := $(shell Y=$$(date +%Y); D=$$(date +%d); Mi=$$(date +%M); Mo=$$(date +%-m); H=$$(date +%-H); echo "$$((Y % 10)) $$D $$Mi $$Mo $$((H + 1))")
endif

WW500_IMG_YEAR_DIGIT = $(word 1,$(WW500_IMG_DATE_RAW))
WW500_IMG_DAY_PAD     = $(word 2,$(WW500_IMG_DATE_RAW))
WW500_IMG_MIN_PAD     = $(word 3,$(WW500_IMG_DATE_RAW))
WW500_IMG_MONTH_NUM   = $(word 4,$(WW500_IMG_DATE_RAW))
WW500_IMG_HOUR_NUM_P1 = $(word 5,$(WW500_IMG_DATE_RAW))

WW500_IMG_MONTH_CHARS = 1 2 3 4 5 6 7 8 9 A B C
WW500_IMG_HOUR_CHARS  = 0 1 2 3 4 5 6 7 8 9 A B C D E F G H I J K L M N
WW500_IMG_MONTH_LETTER = $(word $(WW500_IMG_MONTH_NUM),$(WW500_IMG_MONTH_CHARS))
WW500_IMG_HOUR_LETTER  = $(word $(WW500_IMG_HOUR_NUM_P1),$(WW500_IMG_HOUR_CHARS))

WW500_IMG_DEVICE_STEM = $(WW500_IMG_YEAR_DIGIT)$(WW500_IMG_MONTH_LETTER)$(WW500_IMG_DAY_PAD)$(WW500_IMG_HOUR_LETTER)$(WW500_IMG_MIN_PAD)
WW500_IMG_DEVICE_NAME = $(WW500_IMG_VARIANT_LETTER)$(WW500_IMG_DEVICE_STEM).IMG
WW500_IMG_OUTPUT_DIR = $(WW500_IMAGE_GEN_DIR)/output_case1_sec_wlcsp
WW500_IMG_OUTPUT_DIR_NATIVE = $(subst /,$(PS),$(WW500_IMG_OUTPUT_DIR))

# Device-named images accumulate one per build (the filename is unique down
# to the minute, by design, so nothing here would ever get overwritten on
# its own) - clean up the previous one(s) first so only the latest sticks
# around. Matches only R*.IMG/H*.IMG (this target's own naming pattern);
# output.img and everything else in the folder (json/, inter_files/, etc.)
# are untouched. The leading '-' ignores the "file not found" error Windows'
# del gives on the first-ever run, when there's nothing yet to delete.
ifeq ($(HOST_OS),Windows)
WW500_IMG_CLEAN_CMD = -del /Q $(WW500_IMG_OUTPUT_DIR_NATIVE)\R*.IMG $(WW500_IMG_OUTPUT_DIR_NATIVE)\H*.IMG 2> $(NULL)
else
WW500_IMG_CLEAN_CMD = -rm -f $(WW500_IMG_OUTPUT_DIR)/R*.IMG $(WW500_IMG_OUTPUT_DIR)/H*.IMG 2> $(NULL)
endif

.PHONY: device_image
device_image: gen_image
	$(Q)$(WW500_IMG_CLEAN_CMD)
	@$(ECHO) "Device-named firmware image: " $(WW500_IMG_DEVICE_NAME)
	$(Q)$(CP) $(subst /,$(PS),$(WW500_IMG_OUTPUT_DIR)/output.img) $(subst /,$(PS),$(WW500_IMG_OUTPUT_DIR)/$(WW500_IMG_DEVICE_NAME))

all: device_image

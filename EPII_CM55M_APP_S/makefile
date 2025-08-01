## EPII application makefile template ##

# CGP added this to select folder containing projects
# Uncomment one of the following lines to define the project folder. There must be a corresponding entry in app.mk
#PROJECT_FOLDER = scenario_app
PROJECT_FOLDER = ww_projects

$(info Choosing PROJECT_FOLDER='${PROJECT_FOLDER}' for our projects )

##
# root dir of EPII_ROOT
##
EPII_ROOT = .

# Application name
APPL ?= EPII_CM55M

# Optimization Level
OLEVEL ?= O2
DEBUG ?= 1
##
# Current Board And Core
# BOARD: epii_fpga, epii_evb
##
BOARD ?= epii_evb
BD_VER ?= 10
CORTEX_M ?= 55
#BIG, SMALL
EPII_CORE ?= BIG

##
# IC package select : LQFP128/WLCSP65/QFN88/BGA64
##
IC_PACKAGE_SEL = WLCSP65

##
# IC Version: 10(chipA) 20(chipB)..
##
IC_VER ?= 30

##
# Core Volt 0P9V, 0P8V
##
CoreVolt ?= 0P9V

##
# Set toolchain
# arm, gnu
##
TOOLCHAIN ?= gnu
##
# Definition of Tool Paths
# CGP 10/7/25: It looks like Himax has used GNU tools v10 at some point, and commented out the path.
# I appear to have been using 10.3 from an old ST Micro installation - I have added its path.
# I am downloading 14.2 Rel1 and will add its path also.
# Alternatively, GNU_TOOLPATH could be added as an Eclipse environment variable.
##
ARM_TOOLPATH ?=
GNU_TOOLPATH ?=
#ARM_TOOLPATH ?= C:/Program Files/Arm/Development Studio 2020.1/sw/ARMCompiler6.15/bin
#GNU_TOOLPATH ?= D:/Project/WE-II/CMSIS/cmsis_tool/gcc-arm-none-eabi-10-2020-q4-major-win32/gcc-arm-none-eabi-10-2020-q4-major/bin
GNU_TOOLPATH ?= C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.3 rel1/bin
#GNU_TOOLPATH ?= C:/ST/STM32CubeIDE_1.10.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.10.3-2021.10.win32_1.0.0.202111181127/tools/bin
# CGP - Make location:  C:/ST/STM32CubeIDE_1.10.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.0.100.202202231230/tools/bin

##
# Selected OS
# bare-matal: mark OS_SEL, support os: un-mark OS_SEL
OS_SEL ?= freertos

##
# Selected MPU
# MPU: y or n
MPU ?= y

##
# TrustZone
# TrustZone support y or n
# TRUSTZONE_TYPE security or non-security
# TRUSTZONE_FW_TYPE 0(S and NS) or 1(S only)
TRUSTZONE ?= y
TRUSTZONE_TYPE ?= security
TRUSTZONE_FW_TYPE ?= 0
NONTRUSTZONE_SRC_DIR ?= D:\ARMDS_WORKSPACE\EPII_CM55M_APP_NS



##
# CIS_SENSOR support feature HM_COMMON, HM2140, HM01B0_MONO, HM01B0_BAYER
# HM0360_MONO, HM0360_MONO_OSC, HM0360_BAYER, HM0360_BAYER_OSC, HM11B1, HM11B1_REVC
# HM0360_MONO_REVB, HM0360_MONO_OSC_REVB, HM0360_BAYER_REVB, HM0360_BAYER_OSC_REVB, HM2130, HM2170
##
CIS_SEL =

##
# Platform IP Use Case Selection
# drv_onecore_cm55m_s_only,
# drv_onecore_cm55s_s_only,
# drv_onecore_cm55m_tz_s, drv_onecore_cm55m_tz_ns , 
# drv_onecore_cm55s_tz_s, drv_onecore_cm55s_tz_ns ,
# drv_dualcore_cm55m_s_only, drv_dualcore_cm55s_s_only, 
# drv_dualcore_tz_cm55m_s, drv_dualcore_tz_cm55m_ns, drv_dualcore_tz_cm55s_s, drv_dualcore_tz_cm55s_ns, 
# drv_user_defined 
##
EPII_USECASE_SEL = drv_onecore_cm55m_s_only


EPII_NSC_SEL = cm55m_nsc_cfg

# USING Inference Engine library with CMSIS_NN
# 1 for enable
# other for disable
##
LIB_CMSIS_NN_ENALBE = 1
## 0 : default version (tflmtag2209_u55tag2205)
## 7_0_0 : cmsis_nn_7_0_0 (tflmtag2412_u55tag2411)
LIB_CMSIS_NN_VERSION = 7_0_0

# USING Pre-built CV library
# 1 for enable
# other for disable
##
LIB_CV_PREBUILT = 0

# USING Pre-built Audio Algo library
# 1 for enable
# other for disable
##
LIB_AUDIOALGO_PREBUILT = 0

##
# cmsis_drivers Use Case Selection
# cmsis_drivers_list
##
CMSIS_DRIVERS_USECASE_SEL =

# USING Pre-built Secure crypto library
# y for enable
# other for disable
##
SUPPORT_SEC_LIB ?= y

# customer name for customer specific API
# seeed
##
CUSTOMER ?= seeed

# PS: As follow are scenario_app app name should be the same as directory name
# allon_sensor_tflm
# allon_sensor_tflm_cmsis_nn (LIB_CMSIS_NN_ENALBE = 1)
# allon_sensor_tflm_fatfs
# allon_sensor_tflm_freertos
# allon_jpeg_encode
# fatfs_mmc_spi
# hello_world_freertos_tz_s_only
# tflm_fd_fm
# tflm_yolov8_od
# tflm_yolov8_pose
# pdm_record
# imu_read
# hello_world_cmsis_dsp
# hello_world_cmsis_cv
# kws_pdm_record
# ei_standalone_inferencing
# ei_standalone_inferencing_camera
# edge_impulse_firmware
# tflm_peoplenet
# tflm_yolo11_od
# tflm_yolov8_gender_cls
##
#APP_TYPE = allon_sensor_tflm
 
### Options above must be added before include options.mk ###
override EPII_ROOT := $(strip $(subst \,/,$(EPII_ROOT)))
include $(EPII_ROOT)/options/options.mk

# touch ${APP_TYPE}.c so the __TIME__ and __DATE__ are updated	even if that file is not edited.
# HOWEVER: this does not seem to be working... How can we do it?
${APP_TYPE}.o: ${APP_TYPE}.c timestamp
	$(CC) $(CFLAGS) -c $< -o $@
	
timestamp:
	@touch timestamp
	
# CGP added for infrmation and checking
$(info In makefile APP_TYPE='${APP_TYPE}' SCENARIO_APP_INCDIR='${SCENARIO_APP_INCDIR}')

# Find all .mk files in the project directory and print their full path
# SHELL := powershell.exe
# list_mk_files:
# 	@powershell -File "./list_mk_files.ps1"

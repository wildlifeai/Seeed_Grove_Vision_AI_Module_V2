# directory declaration
LIB_LIBEXIF_DIR = $(LIBRARIES_ROOT)/libexif

LIB_LIBEXIF_ASMSRCDIR	= $(LIB_LIBEXIF_DIR)
LIB_LIBEXIF_CSRCDIR	= $(LIB_LIBEXIF_DIR)
LIB_LIBEXIF_CXXSRCSDIR    = $(LIB_LIBEXIF_DIR)
LIB_LIBEXIF_INCDIR	= $(LIB_LIBEXIF_DIR) $(LIB_LIBEXIF_DIR)/seconly_inc 

# find all the source files in the target directories
LIB_LIBEXIF_CSRCS = $(call get_csrcs, $(LIB_LIBEXIF_CSRCDIR))
LIB_LIBEXIF_CXXSRCS = $(call get_cxxsrcs, $(LIB_LIBEXIF_CXXSRCSDIR))
LIB_LIBEXIF_ASMSRCS = $(call get_asmsrcs, $(LIB_LIBEXIF_ASMSRCDIR))

# get object files
LIB_LIBEXIF_COBJS = $(call get_relobjs, $(LIB_LIBEXIF_CSRCS))
LIB_LIBEXIF_CXXOBJS = $(call get_relobjs, $(LIB_LIBEXIF_CXXSRCS))
LIB_LIBEXIF_ASMOBJS = $(call get_relobjs, $(LIB_LIBEXIF_ASMSRCS))
LIB_LIBEXIF_OBJS = $(LIB_LIBEXIF_COBJS) $(LIB_LIBEXIF_ASMOBJS) $(LIB_LIBEXIF_CXXOBJS)

# get dependency files
LIB_LIBEXIF_DEPS = $(call get_deps, $(LIB_LIBEXIF_OBJS))

# extra macros to be defined
LIB_LIBEXIF_DEFINES = -DLIB_LIBEXIF

# genearte library
ifeq ($(LIBEXIF_LIB_FORCE_PREBUILT), y)
override LIB_LIBEXIF_OBJS:=
endif
LIBEXIF_LIB_NAME = libexif.a
LIB_LIB_LIBEXIF := $(subst /,$(PS), $(strip $(OUT_DIR)/$(LIBEXIF_LIB_NAME)))

# library generation rule
$(LIB_LIB_LIBEXIF): $(LIB_LIBEXIF_OBJS)
	$(TRACE_ARCHIVE)
ifeq "$(strip $(LIB_LIBEXIF_OBJS))" ""
	$(CP) $(PREBUILT_LIB)$(LIBEXIF_LIB_NAME) $(LIB_LIB_LIBEXIF)
else
	$(Q)$(AR) $(AR_OPT) $@ $(LIB_LIBEXIF_OBJS)
	$(CP) $(LIB_LIB_LIBEXIF) $(PREBUILT_LIB)$(LIBEXIF_LIB_NAME)
endif


# specific compile rules
# user can add rules to compile this middleware
# if not rules specified to this middleware, it will use default compiling rules

# Middleware Definitions
LIB_INCDIR += $(LIB_LIBEXIF_INCDIR)
LIB_CSRCDIR += $(LIB_LIBEXIF_CSRCDIR)
LIB_CXXSRCDIR += $(LIB_LIBEXIF_CXXSRCDIR)
LIB_ASMSRCDIR += $(LIB_LIBEXIF_ASMSRCDIR)

LIB_CSRCS += $(LIB_LIBEXIF_CSRCS)
LIB_CXXSRCS += $(LIB_LIBEXIF_CXXSRCS)
LIB_ASMSRCS += $(LIB_LIBEXIF_ASMSRCS)
LIB_ALLSRCS += $(LIB_LIBEXIF_CSRCS) $(LIB_LIBEXIF_ASMSRCS)

LIB_COBJS += $(LIB_LIBEXIF_COBJS)
LIB_CXXOBJS += $(LIB_LIBEXIF_CXXOBJS)
LIB_ASMOBJS += $(LIB_LIBEXIF_ASMOBJS)
LIB_ALLOBJS += $(LIB_LIBEXIF_OBJS)

LIB_DEFINES += $(LIB_LIBEXIF_DEFINES)
LIB_DEPS += $(LIB_LIBEXIF_DEPS)
LIB_LIBS += $(LIB_LIB_LIBEXIF)
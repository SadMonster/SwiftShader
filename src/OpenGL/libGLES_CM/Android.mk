LOCAL_PATH:= $(call my-dir)

COMMON_CFLAGS := \
	-DLOG_TAG=\"libGLES_CM_swiftshader\" \
	-std=c++11 \
	-fno-operator-names \
	-msse2 \
	-D__STDC_CONSTANT_MACROS \
	-D__STDC_LIMIT_MACROS \
	-DEGLAPI= \
	-DGL_API= \
	-DGL_APICALL= \
	-DGL_GLEXT_PROTOTYPES \
	-Wno-unused-parameter \
	-Wno-implicit-exception-spec-mismatch \
	-Wno-overloaded-virtual \
	-DANDROID_PLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

ifneq (16,${PLATFORM_SDK_VERSION})
COMMON_CFLAGS += -Xclang -fuse-init-array
else
COMMON_CFLAGS += -D__STDC_INT64__
endif

COMMON_SRC_FILES := \
	Buffer.cpp \
	Context.cpp \
	Device.cpp \
	Framebuffer.cpp \
	IndexDataManager.cpp \
	libGLES_CM.cpp \
	main.cpp \
	Renderbuffer.cpp \
	ResourceManager.cpp \
	Texture.cpp \
	utilities.cpp \
	VertexDataManager.cpp

COMMON_C_INCLUDES := \
	bionic \
	$(LOCAL_PATH)/../../../include \
	$(LOCAL_PATH)/../ \
	$(LOCAL_PATH)/../../ \
	$(LOCAL_PATH)/../../Renderer/ \
	$(LOCAL_PATH)/../../Common/ \
	$(LOCAL_PATH)/../../Shader/ \
	$(LOCAL_PATH)/../../Main/

ifdef use_subzero
COMMON_STATIC_LIBRARIES := libsubzero
else
COMMON_STATIC_LIBRARIES := libLLVM_swiftshader
endif

COMMON_SHARED_LIBRARIES := \
	libdl \
	liblog \
	libcutils \
	libhardware

# gralloc1 is introduced from N MR1
ifeq ($(shell test $(PLATFORM_SDK_VERSION) -ge 25 && echo NMR1),NMR1)
COMMON_CFLAGS += -DHAVE_GRALLOC1
COMMON_SHARED_LIBRARIES += libsync
endif

# Marshmallow does not have stlport, but comes with libc++ by default
ifeq ($(shell test $(PLATFORM_SDK_VERSION) -lt 23 && echo PreMarshmallow),PreMarshmallow)
COMMON_SHARED_LIBRARIES += libstlport
COMMON_C_INCLUDES += external/stlport/stlport
endif

COMMON_LDFLAGS := \
	-Wl,--gc-sections \
	-Wl,--version-script=$(LOCAL_PATH)/exports.map \
	-Wl,--hash-style=sysv

include $(CLEAR_VARS)
LOCAL_MODULE := libGLESv1_CM_swiftshader_debug
ifdef TARGET_2ND_ARCH
ifeq ($(TARGET_TRANSLATE_2ND_ARCH),true)
LOCAL_MULTILIB := first
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/egl
else
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib/egl
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64/egl
endif
else
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/egl
endif
LOCAL_MODULE_TAGS := optional
LOCAL_CLANG := true
LOCAL_SRC_FILES += $(COMMON_SRC_FILES)
LOCAL_C_INCLUDES += $(COMMON_C_INCLUDES)
LOCAL_STATIC_LIBRARIES += swiftshader_top_debug $(COMMON_STATIC_LIBRARIES)
LOCAL_SHARED_LIBRARIES += $(COMMON_SHARED_LIBRARIES)
LOCAL_LDFLAGS += $(COMMON_LDFLAGS)
LOCAL_CFLAGS += $(COMMON_CFLAGS) -UNDEBUG -g -O0
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libGLESv1_CM_swiftshader
ifdef TARGET_2ND_ARCH
ifeq ($(TARGET_TRANSLATE_2ND_ARCH),true)
LOCAL_MULTILIB := first
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/egl
else
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib/egl
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64/egl
endif
else
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/egl
endif
LOCAL_MODULE_TAGS := optional
LOCAL_CLANG := true
LOCAL_SRC_FILES += $(COMMON_SRC_FILES)
LOCAL_C_INCLUDES += $(COMMON_C_INCLUDES)
LOCAL_STATIC_LIBRARIES += swiftshader_top_release $(COMMON_STATIC_LIBRARIES)
LOCAL_SHARED_LIBRARIES += $(COMMON_SHARED_LIBRARIES)
LOCAL_LDFLAGS += $(COMMON_LDFLAGS)
LOCAL_CFLAGS += \
	$(COMMON_CFLAGS) \
	-fomit-frame-pointer \
	-ffunction-sections \
	-fdata-sections \
	-DANGLE_DISABLE_TRACE
include $(BUILD_SHARED_LIBRARY)

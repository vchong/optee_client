################################################################################
# Android optee-client, libsks and tee-supplicant makefile                     #
################################################################################
LOCAL_PATH := $(call my-dir)

# set CFG_TEE_CLIENT_LOAD_PATH before include config.mk
CFG_TEE_CLIENT_LOAD_PATH ?= /system/lib

################################################################################
# Include optee-client common config and flags                                 #
################################################################################
include $(LOCAL_PATH)/config.mk
include $(LOCAL_PATH)/android_flags.mk

optee_CFLAGS = $(CFLAGS)

################################################################################
# Build libteec.so - TEE (Trusted Execution Environment) shared library        #
################################################################################
include $(CLEAR_VARS)
LOCAL_CFLAGS += $(optee_CFLAGS)

ifneq ($(CFG_TEE_CLIENT_LOG_FILE),)
LOCAL_CFLAGS += -DTEEC_LOG_FILE=$(CFG_TEE_CLIENT_LOG_FILE)
endif

LOCAL_CFLAGS += -DDEBUGLEVEL_$(CFG_TEE_CLIENT_LOG_LEVEL)
LOCAL_CFLAGS += -DBINARY_PREFIX=\"TEEC\"

LOCAL_SRC_FILES := libteec/src/tee_client_api.c \
                   libteec/src/teec_trace.c
ifeq ($(CFG_TEE_BENCHMARK),y)
LOCAL_CFLAGS += -DCFG_TEE_BENCHMARK
LOCAL_SRC_FILES += teec_benchmark.c
endif

LOCAL_C_INCLUDES := $(LOCAL_PATH)/public \
                    $(LOCAL_PATH)/libteec/include

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := libteec
LOCAL_MODULE_TAGS := optional

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/public

include $(BUILD_SHARED_LIBRARY)

# Build libsks, i.e. liboptee_cryptoki.so
include $(CLEAR_VARS)

LOCAL_CFLAGS += $(optee_CFLAGS)

LOCAL_SRC_FILES := libsks/src/pkcs11_api.c \
                   libsks/src/ck_debug.c \
                   libsks/src/ck_helpers.c \
                   libsks/src/invoke_ta.c \
                   libsks/src/pkcs11_token.c \
                   libsks/src/serializer.c \
                   libsks/src/serialize_ck.c \
                   libsks/src/pkcs11_processing.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/public \
                    $(LOCAL_PATH)/libsks/include

LOCAL_SHARED_LIBRARIES := libteec
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := liboptee_cryptoki
LOCAL_MODULE_TAGS := optional

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/public

include $(BUILD_SHARED_LIBRARY)

# TEE Supplicant
include $(LOCAL_PATH)/tee-supplicant/tee_supplicant_android.mk

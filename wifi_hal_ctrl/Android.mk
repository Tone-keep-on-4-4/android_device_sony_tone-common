LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_CFLAGS := -Wno-unused-parameter
LOCAL_CFLAGS += -Wall -Werror
LOCAL_MODULE := libwifi-hal-ctrl
LOCAL_VENDOR_MODULE := true
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_SRC_FILES := wifi_hal_ctrl.c
LOCAL_HEADER_LIBRARIES := libcutils_headers
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libwifi-hal-ctrl_headers
LOCAL_EXPORT_C_INCLUDE_DIRS := wifi_hal_ctrl
LOCAL_HEADER_LIBRARIES := libcutils_headers
include $(BUILD_HEADER_LIBRARY)

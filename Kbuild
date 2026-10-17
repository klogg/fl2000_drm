# SPDX-License-Identifier: GPL-2.0
fl2000-y := \
	fl2000_drv.o \
	fl2000_registers.o \
	fl2000_interrupt.o \
	fl2000_streaming.o \
	fl2000_i2c.o \
	fl2000_drm.o

it66121-y := \
	bridge/it66121_drv.o

obj-$(CONFIG_DRM_FL2000) += fl2000.o
obj-$(CONFIG_DRM_FL2000_IT66121) += it66121.o

# Add new architecture backends here; generic always retains the scalar path.
ifneq ($(KBUILD_EXTMOD),)
obj-m := fl2000.o it66121.o
FL2000_FRAME_BACKEND ?= auto
fl2000_frame_backend := $(FL2000_FRAME_BACKEND)
ifeq ($(fl2000_frame_backend),auto)
ifeq ($(CONFIG_X86_64),y)
fl2000_frame_backend := ssse3
else
fl2000_frame_backend := generic
endif
endif
else
fl2000_frame_backend := generic
ifeq ($(CONFIG_DRM_FL2000_FRAME_SSSE3),y)
fl2000_frame_backend := ssse3
else ifeq ($(CONFIG_DRM_FL2000_FRAME_BMI2),y)
fl2000_frame_backend := bmi2
endif
endif
ifeq ($(fl2000_frame_backend),ssse3)
ifneq ($(CONFIG_X86_64),y)
$(error SSSE3 frame backend requires an x86-64 target kernel)
endif
ifneq ($(CONFIG_ARCH_HAS_KERNEL_FPU_SUPPORT),y)
$(error SSSE3 frame backend requires kernel FPU support)
endif
fl2000-y += frame/x86.o frame/ssse3.o
ccflags-y += -DFL2000_FRAME_ACCELERATED
CFLAGS_REMOVE_frame/ssse3.o += $(CC_FLAGS_NO_FPU)
CFLAGS_frame/ssse3.o += $(CC_FLAGS_FPU) -mssse3 -mno-avx
else ifeq ($(fl2000_frame_backend),bmi2)
ifneq ($(CONFIG_X86_64),y)
$(error BMI2 frame backend requires an x86-64 target kernel)
endif
fl2000-y += frame/x86_bmi2.o frame/bmi2.o
ccflags-y += -DFL2000_FRAME_ACCELERATED
CFLAGS_frame/bmi2.o += -mbmi2
else ifneq ($(fl2000_frame_backend),generic)
$(error Unknown FL2000_FRAME_BACKEND $(FL2000_FRAME_BACKEND))
endif

# External builds may reuse unmodified sources for helpers absent from the kernel.
ifneq ($(KBUILD_EXTMOD),)
FL2000_HELPER_SOURCE_DIR ?= $(src)/.qemu-build/upstream
ifeq ($(CONFIG_DRM_BRIDGE_CONNECTOR),)
fl2000-y += compat/bridge_connector.o
endif
ifeq ($(CONFIG_DRM_DISPLAY_HDMI_STATE_HELPER),)
fl2000-y += compat/hdmi_state.o
endif
ifeq ($(CONFIG_DRM_DISPLAY_HDMI_AUDIO_HELPER),)
fl2000-y += compat/hdmi_audio.o
endif
ifeq ($(CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER),)
fl2000-y += compat/hdmi_cec.o
endif
CFLAGS_compat/bridge_connector.o += -DFL2000_HELPER_SOURCE='"$(abspath $(FL2000_HELPER_SOURCE_DIR))/drm_bridge_connector.c"'
CFLAGS_compat/hdmi_state.o += -DFL2000_HELPER_SOURCE='"$(abspath $(FL2000_HELPER_SOURCE_DIR))/drm_hdmi_state_helper.c"'
CFLAGS_compat/hdmi_audio.o += -DFL2000_HELPER_SOURCE='"$(abspath $(FL2000_HELPER_SOURCE_DIR))/drm_hdmi_audio_helper.c"'
CFLAGS_compat/hdmi_cec.o += -DFL2000_HELPER_SOURCE='"$(abspath $(FL2000_HELPER_SOURCE_DIR))/drm_hdmi_cec_helper.c"'
endif

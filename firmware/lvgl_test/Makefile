SDK_DEMO_PATH ?= $(abspath .)

# 本机 SDK 路径：环境变量 BL_SDK_BASE，或 firmware/dev-env.mk（不入库，见 README）
-include ../dev-env.mk
BL_SDK_BASE ?= $(error BL_SDK_BASE 未设置：导出环境变量，或在 firmware/dev-env.mk 里配置)

export BL_SDK_BASE

include $(BL_SDK_BASE)/project.build

ARTIFACT = SafetyTask

PLATFORM ?= aarch64le
BUILD_PROFILE ?= debug
CONFIG_NAME ?= $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR := build/$(CONFIG_NAME)
TARGET := $(OUTPUT_DIR)/$(ARTIFACT)

CC := qcc -Vgcc_nto$(PLATFORM)
LD := $(CC)

INCLUDES += -Iinclude -I../common/include

CCFLAGS_all += -Wall -Wextra -fmessage-length=0
CCFLAGS_all += $(CCFLAGS_$(BUILD_PROFILE))

CCFLAGS_release += -O2
CCFLAGS_debug += -g -O0 -fno-builtin
CCFLAGS_coverage += -g -O0 -ftest-coverage -fprofile-arcs
CCFLAGS_profile += -g -O0 -finstrument-functions

DEPS := -Wp,-MMD,$(@:%.o=%.d),-MT,$@
SRCS := $(wildcard src/*.c)
OBJS := $(addprefix $(OUTPUT_DIR)/,$(SRCS:.c=.o))

$(OUTPUT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(DEPS) -o $@ $(INCLUDES) $(CCFLAGS_all) $<

$(TARGET): $(OBJS)
	$(LD) -o $@ $(OBJS)

.PHONY: all clean rebuild

all: $(TARGET)

clean:
	rm -rf build

rebuild: clean all

-include $(OBJS:.o=.d)

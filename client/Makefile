# client/Makefile — builds the PC-side binaries: esp_daemon and ap_provision
#
# Expected layout (this Makefile lives in client/):
#   client/source/    main.c, netlink_monitor.c, ap_provision.c
#   client/include/   main.h, netlink_monitor.h
#   common/           common.c, common.h          (one level up: ../common)
#
# Binaries (land in client/build/):
#   esp_daemon    = main.c + netlink_monitor.c + common.c   (needs -lpthread)
#   ap_provision  = ap_provision.c + common.c               (needs OpenSSL)
#
# Targets:
#   make            build both (default)
#   make daemon     build only esp_daemon
#   make prov       build only ap_provision
#   make clean      remove build/

CC      ?= gcc
CSTD    := -std=gnu11
WARN    := -Wall -Wextra -Wno-missing-braces -Wno-unused-parameter
OPT     := -O2 -g

# ---- layout --------------------------------------------------------------
SRC_DIR    := source
INC_DIR    := include
COMMON_DIR := ../common
BUILD      := build

INCLUDES := -I$(SRC_DIR) -I$(INC_DIR) -I$(COMMON_DIR)
CFLAGS   := $(CSTD) $(WARN) $(OPT) $(INCLUDES)

# ---- sources -> objects --------------------------------------------------
COMMON_SRC := $(COMMON_DIR)/common.c

DAEMON_SRC := $(SRC_DIR)/main.c $(SRC_DIR)/netlink_monitor.c $(COMMON_SRC)
PROV_SRC   := $(SRC_DIR)/ap_provision.c $(COMMON_SRC)

DAEMON_OBJ := $(addprefix $(BUILD)/,$(notdir $(DAEMON_SRC:.c=.o)))
PROV_OBJ   := $(addprefix $(BUILD)/,$(notdir $(PROV_SRC:.c=.o)))

# ---- external libs -------------------------------------------------------
DAEMON_LIBS := -lpthread

# OpenSSL: ask pkg-config, fall back to the usual link flags if it's absent
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)
ifeq ($(strip $(OPENSSL_LIBS)),)
  OPENSSL_LIBS := -lssl -lcrypto
endif

# ---- targets -------------------------------------------------------------
.PHONY: all daemon prov clean
all: daemon prov

daemon: $(BUILD)/esp_daemon
prov:   $(BUILD)/ap_provision

$(BUILD):
	@mkdir -p $(BUILD)

# link: daemon
$(BUILD)/esp_daemon: $(DAEMON_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(DAEMON_LIBS)
	@echo "  [client] built $@"

# link: provisioning tool
$(BUILD)/ap_provision: $(PROV_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(OPENSSL_LIBS)
	@echo "  [client] built $@"

# compile: ap_provision is the only unit that includes <openssl/*>
$(BUILD)/ap_provision.o: $(SRC_DIR)/ap_provision.c | $(BUILD)
	$(CC) $(CFLAGS) $(OPENSSL_CFLAGS) -c $< -o $@

# compile: everything else under source/
$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# compile: common.c (lives outside source/, so it needs its own rule)
$(BUILD)/common.o: $(COMMON_DIR)/common.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
	@echo "  [client] cleaned"

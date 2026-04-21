APP_NAME           = "Remote Desktop"
BIN                = rdp-server
LABEL              = org.example.rdp
PLIST              = org.example.rdp.plist
INFO_PLIST         = packaging/Info.plist

APP_DIR           ?= $(HOME)/Applications
PLIST_DIR          = $(HOME)/Library/LaunchAgents
APP_BUNDLE         = $(APP_DIR)/$(APP_NAME).app
BUILD_BUNDLE       = _build/$(APP_NAME).app
EXEC_PATH          = $(APP_BUNDLE)/Contents/MacOS/$(BIN)

CODESIGN_IDENTITY ?= Morgan

.PHONY: build run bundle sign install uninstall load unload reload clean

build:
	@if [ ! -d _build ]; then meson setup _build; fi
	meson compile -C _build

bundle: build
	@rm -rf $(BUILD_BUNDLE)
	@mkdir -p $(BUILD_BUNDLE)/Contents/MacOS
	@cp $(INFO_PLIST) $(BUILD_BUNDLE)/Contents/Info.plist
	@cp _build/$(BIN) $(BUILD_BUNDLE)/Contents/MacOS/$(BIN)
	@echo "Built bundle: $(BUILD_BUNDLE)"

sign: bundle
	@codesign --force --sign "$(CODESIGN_IDENTITY)" $(BUILD_BUNDLE)
	@codesign --verify --verbose=2 $(BUILD_BUNDLE)
	@codesign -dv $(BUILD_BUNDLE) 2>&1 | grep -E '^(Identifier|Authority|TeamIdentifier)='

install: sign
	@mkdir -p $(APP_DIR) $(PLIST_DIR)
	@rm -rf $(APP_BUNDLE)
	@cp -R $(BUILD_BUNDLE) $(APP_BUNDLE)
	@sed 's|__EXEC__|"$(EXEC_PATH)"|' $(PLIST) > $(PLIST_DIR)/$(PLIST)
	@echo "Installed $(APP_BUNDLE)"
	@echo "Installed $(PLIST_DIR)/$(PLIST)"

uninstall: unload
	@rm -rf $(APP_BUNDLE)
	@rm -f $(PLIST_DIR)/$(PLIST)
	@echo "Uninstalled."

load:
	@launchctl bootstrap gui/$$(id -u) $(PLIST_DIR)/$(PLIST) || \
	@launchctl kickstart -k gui/$$(id -u)/$(LABEL) || \
	@echo "Started $(LABEL)"

unload:
	@launchctl bootout gui/$$(id -u)/$(LABEL) 2>/dev/null || true
	@echo "Stopped $(LABEL)"

reload: unload load

clean:
	@rm -rf _build

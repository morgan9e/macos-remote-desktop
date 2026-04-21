PREFIX ?= $(HOME)/.local
PLIST_DIR = $(HOME)/Library/LaunchAgents
PLIST = org.example.rdp.plist
BIN = rdp-server
LABEL = org.example.rdp

.PHONY: build install uninstall load unload reload logs clean

build:
	@cd build && ninja

install: build
	@mkdir -p $(PREFIX)/bin
	@cp build/macos-rdp-server $(PREFIX)/bin/$(BIN)
	@codesign --force --sign - $(PREFIX)/bin/$(BIN)
	@cp $(PLIST) $(PLIST_DIR)/$(PLIST)
	@echo "Installed $(PREFIX)/bin/$(BIN)"
	@echo "Installed $(PLIST_DIR)/$(PLIST)"

uninstall: unload
	@rm -f $(PREFIX)/bin/$(BIN)
	@rm -f $(PLIST_DIR)/$(PLIST)
	@echo "Uninstalled."

load:
	@launchctl bootstrap gui/$$(id -u) $(PLIST_DIR)/$(PLIST) 2>/dev/null || true
	@launchctl kickstart -k gui/$$(id -u)/$(LABEL)
	@echo "Started $(LABEL)"

unload:
	@launchctl bootout gui/$$(id -u)/$(LABEL) 2>/dev/null || true
	@echo "Stopped $(LABEL)"

reload: unload load

logs:
	@tail -f /tmp/rdp-server.log

clean:
	@cd build && ninja clean

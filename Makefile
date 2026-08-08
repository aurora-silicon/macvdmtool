CXXFLAGS := -std=c++14 -Wall -Wextra -Wpedantic
LDFLAGS := -framework CoreFoundation -framework IOKit -lc++

OBJS := main.o
SERVICE_BINS := macvdmd macvdmctl

all: macvdmtool $(SERVICE_BINS)

macvdmtool: $(OBJS)
	cc -o $@ $(OBJS) $(LDFLAGS)

macvdmd: macvdmd.o
	cc -o $@ $< -lc++

macvdmctl: macvdmctl.o
	cc -o $@ $< -lc++

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

INSTALL := /usr/bin/install

.PHONY: install
install: macvdmtool
	@sudo $(INSTALL) -d "$(PREFIX)/bin/"
	@sudo $(INSTALL) -m 755 -o root -g wheel "$(<)" "$(PREFIX)/bin/"

SERVICE_USER ?= $(shell id -un)

.PHONY: install-service
install-service: all
	@sudo ./install-service.sh "$(SERVICE_USER)" "$(PREFIX)"

.PHONY: uninstall-service
uninstall-service:
	@sudo ./uninstall-service.sh "$(PREFIX)"

.PHONY: clean
clean:
	rm -f $(OBJS) macvdmd.o macvdmctl.o macvdmtool $(SERVICE_BINS)

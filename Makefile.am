# reposync — top-level: recursive control of src/ (CLI) and gui/ (SDL3 GUI)
# Build: autoreconf -i && ./configure && make   (make distclean to wipe all)
SUBDIRS = src
if BUILD_GUI
SUBDIRS += gui
endif

# convenience target kept for the test scripts / workflows
cli:
	$(MAKE) -C src

.PHONY: cli

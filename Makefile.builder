ifeq ($(PACKAGE_SET),vm)
WIN_COMPILER := mingw
WIN_SOURCE_SUBDIRS= win
#WIN_PREBUILD_CMD = set_version.bat
WIN_PACKAGE_CMD = true
WIN_BUILD_DEPS = core-vchan-$(BACKEND_VMM)
ifeq ($(BACKEND_VMM),xen)
WIN_BUILD_DEPS += vmm-xen-windows-pvdrivers
endif
endif


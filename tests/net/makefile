include prorab.mk

this_name := tests

this_out_dir := build

this_no_install := true

this_cxxflags += -std=c++11
this_cxxflags += -Wall
this_cxxflags += -fstrict-aliasing

ifeq ($(debug),true)
    this_cxxflags += -DDEBUG
else
    this_cxxflags += -O3
endif

this_srcs += main.cpp socket.cpp dns.cpp

this_ldlibs += -lsetka -lnitki -lpapki -lopros -lutki

ifeq ($(os),macosx)
    this_cxxflags += -stdlib=libc++ # this is needed to be able to use c++11 std lib
    this_ldlibs += -lc++
else ifeq ($(os),windows)
    this_ldlibs += -lws2_32
else
    this_ldlibs += -lpthread
endif

this_ldflags += -L$(d)../../src/build/

this_ldlibs += -lstdc++

$(eval $(prorab-build-app))

include $(d)../test_target.mk

# add dependency on libsetka.so
$(prorab_this_name): $(abspath $(d)../../src/build/libsetka$(dot_so))

# include makefile for building setka
$(eval $(call prorab-include, ../../src/makefile))

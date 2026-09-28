# pw-video-simple-interface -- static libraries + examples
#
# core   : the PipeWire video node. Dependencies: libpipewire-0.3.
# extras : cairo/pango/gdk-pixbuf/curl helpers built on top of the core (frame bridging, text
#          with an outline, an image cache, an HTTP client). Built only when those libraries are
#          installed, so a consumer that only wants the video node does not need them.

CXX   ?= g++
PKGS       := libpipewire-0.3
PKGS_EXTRAS := cairo pangocairo gdk-pixbuf-2.0 libcurl

CXXFLAGS ?= -O3 -g
CXXFLAGS += -std=c++20 -Wall -Wextra $(EXTRA_CXXFLAGS)

CFLAGS_CORE   := $(CXXFLAGS) $(shell pkg-config --cflags $(PKGS))
CFLAGS_EXTRAS := $(CFLAGS_CORE) $(shell pkg-config --cflags $(PKGS_EXTRAS))
LIBS_CORE     := $(shell pkg-config --libs $(PKGS))
LIBS_EXTRAS   := $(shell pkg-config --libs $(PKGS_EXTRAS))

LIB        := libpwvideo.a
LIB_EXTRAS := libpwvideo-cairo.a
CORE_OBJ   := $(patsubst %.cpp,%.o,$(wildcard src/*.cpp))
EXTRA_OBJ  := $(patsubst %.cpp,%.o,$(wildcard extras/*.cpp))

HAVE_EXTRAS := $(shell pkg-config --exists $(PKGS_EXTRAS) && echo 1)

.PHONY: all core extras clean run run-extras

ifeq ($(HAVE_EXTRAS),1)
all: core extras
extras: $(LIB_EXTRAS) demo-cairo
else
all: core
$(info extras not built: install cairo, pangocairo, gdk-pixbuf2 and libcurl)
endif

core: $(LIB) demo

$(LIB): $(CORE_OBJ)
	ar rcs $@ $^

$(LIB_EXTRAS): $(EXTRA_OBJ) $(LIB)
	ar rcs $@ $^

src/%.o: src/%.cpp
	$(CXX) $(CFLAGS_CORE) -MMD -MP -c -o $@ $<

extras/%.o: extras/%.cpp
	$(CXX) $(CFLAGS_EXTRAS) -MMD -MP -c -o $@ $<

examples/%.o: examples/%.cpp
	$(CXX) $(CFLAGS_EXTRAS) -I src -I extras -MMD -MP -c -o $@ $<

demo: examples/demo.o $(LIB)
	$(CXX) $(CFLAGS_CORE) -o $@ examples/demo.o $(LIB) $(LIBS_CORE)

demo-cairo: examples/demo-cairo.o $(LIB_EXTRAS) $(LIB)
	$(CXX) $(CFLAGS_EXTRAS) -o $@ examples/demo-cairo.o $(LIB_EXTRAS) $(LIB) \
	    $(LIBS_EXTRAS) $(LIBS_CORE)

run: demo
	./$(DEMO)

DEMO ?= demo

run-extras: demo-cairo
	./demo-cairo

clean:
	rm -f $(CORE_OBJ) $(EXTRA_OBJ) $(CORE_OBJ:.o=.d) $(EXTRA_OBJ:.o=.d) \
	    examples/*.o examples/*.d $(LIB) $(LIB_EXTRAS) demo demo-cairo

-include $(CORE_OBJ:.o=.d) $(EXTRA_OBJ:.o=.d) examples/*.d

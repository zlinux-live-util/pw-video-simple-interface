# pw-video-simple-interface —— 静态库 + 示例
# 依赖只有 PipeWire 开发头（发行版系统库）。

CXX      ?= g++
PKGS     := libpipewire-0.3
CXXFLAGS ?= -O3 -g
CXXFLAGS += -std=c++20 -Wall -Wextra $(EXTRA_CXXFLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

LIB      := libpwvideo.a
SRC      := src/pwvideo.cpp
OBJ      := $(SRC:.cpp=.o)
DEP      := $(OBJ:.o=.d)
DEMO     := demo

.PHONY: all clean run

all: $(LIB) $(DEMO)

$(LIB): $(OBJ)
	ar rcs $@ $^

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(DEMO): examples/demo.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ examples/demo.o $(LIB) $(LDLIBS)

examples/%.o: examples/%.cpp
	$(CXX) $(CXXFLAGS) -I src -MMD -MP -c -o $@ $<

run: $(DEMO)
	./$(DEMO)

clean:
	rm -f $(OBJ) $(DEP) examples/*.o examples/*.d $(LIB) $(DEMO)

-include $(DEP) examples/*.d

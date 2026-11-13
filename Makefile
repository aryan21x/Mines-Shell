CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -g

.PHONY: all test test-sanitize clean

all: mish

mish: Main.cpp mish.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) Main.cpp $(LDFLAGS) -o $@

mish-sanitize: Main.cpp mish.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-omit-frame-pointer Main.cpp $(LDFLAGS) -o $@

test: mish
	MISH_BINARY="$(CURDIR)/mish" python3 test_mish.py -v

test-sanitize: mish-sanitize
	MISH_BINARY="$(CURDIR)/mish-sanitize" python3 test_mish.py -v

clean:
	$(RM) mish mish-sanitize

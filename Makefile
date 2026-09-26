CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror -Iinclude
SRCS := src/builder.cpp src/view.cpp
TEST_SRC := tests/test_main.cpp

.PHONY: test asan clean

test: build/widetree_tests
	./build/widetree_tests

build/widetree_tests: $(SRCS) $(TEST_SRC) include/widetree/*.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(TEST_SRC)

asan:
	mkdir -p build
	$(CXX) -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
		-o build/widetree_tests_asan $(SRCS) $(TEST_SRC)
	./build/widetree_tests_asan

clean:
	rm -rf build

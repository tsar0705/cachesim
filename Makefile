CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
SRC      := src/cache.cpp src/trace.cpp

.PHONY: all test diff sweep asan clean
all: build/cachesim

build/cachesim: $(SRC) src/main.cpp src/cache.hpp src/trace.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) src/main.cpp -o $@

build/test_cache: $(SRC) tests/test_cache.cpp src/cache.hpp src/trace.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc $(SRC) tests/test_cache.cpp -o $@

test: build/test_cache
	./build/test_cache

diff: build/cachesim
	python3 tools/difftest.py --cases 300 --seed 1

sweep: build/cachesim
	python3 tools/sweep.py

# Everything again under ASan+UBSan
asan:
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra $(SAN) $(SRC) src/main.cpp -o build/cachesim_asan
	$(CXX) -std=c++17 -Wall -Wextra $(SAN) -Isrc $(SRC) tests/test_cache.cpp -o build/test_cache_asan
	./build/test_cache_asan
	python3 tools/difftest.py --cases 150 --seed 2 --bin build/cachesim_asan

clean:
	rm -rf build results/*.csv results/*.tmp

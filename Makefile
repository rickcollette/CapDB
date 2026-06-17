# Top-level convenience targets. Configure with CMake first:
#   cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON

.PHONY: all build clean distclean test

all build:
	cmake --build build -j$$(nproc)

clean:
	@./scripts/clean.sh clean

distclean:
	@./scripts/clean.sh distclean

test:
	cd build && ctest --output-on-failure

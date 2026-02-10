FROM node:25.6.0-bookworm

ENV CMAKE_BUILD_PARALLEL_LEVEL=32 MAKEFLAGS=-j32 JOBS=32 DEBUG_LEVEL=0

# Install build dependencies for gcc
RUN apt update && apt install -y build-essential libgmp-dev libmpfr-dev libmpc-dev flex bison wget

RUN apt update && apt install liburing-dev cmake pip -y

# Let pip write to system site-packages (Debian 12 + PEP 668)
ENV PIP_BREAK_SYSTEM_PACKAGES=1

RUN git clone --depth 1 --branch liburing-2.14 https://github.com/axboe/liburing.git /tmp/liburing && \
    cd /tmp/liburing && ./configure && make -j"$(nproc)" && make install && ldconfig

# Clone and build folly
RUN apt update && apt install sudo -y
RUN mkdir -p /opt/folly && cd /opt/folly && \
  git clone --depth 1 --branch v2026.02.02.00 https://github.com/facebook/folly . && \
  ./build/fbcode_builder/getdeps.py install-system-deps --recursive && \
  ./build/fbcode_builder/getdeps.py build --no-tests \
  --extra-cmake-defines='{"CMAKE_BUILD_TYPE":"Release", "CMAKE_C_FLAGS":"-march=znver3 -mtune=znver3 -O3 -fPIC", "CMAKE_CXX_FLAGS":"-march=znver3 -mtune=znver3 -O3 -fPIC" }'

# Copy folly (lib + headers + boost) into system folder
RUN cd `cd /opt/folly && ./build/fbcode_builder/getdeps.py show-inst-dir folly` && \
  cp lib/libfolly.a /usr/lib/x86_64-linux-gnu/ && \
  cp -rv include/ /usr/lib/x86_64-linux-gnu && \
  cp -rv ../boost*/include/ /usr/lib/x86_64-linux-gnu

RUN cd /opt && git clone https://github.com/fmtlib/fmt.git && cd fmt && \
  cmake -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE . && \
  make && \
  cp -rv include/ /usr/lib/x86_64-linux-gnu && \
  cp libfmt.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone https://github.com/google/glog.git && cd glog && \
  cmake -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE -DBUILD_SHARED_LIBS=FALSE . && \
  make && \
  cp libglog.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone https://github.com/libunwind/libunwind.git && cd libunwind && \
  autoreconf -i && ./configure CFLAGS="-fPIC" CXXFLAGS="-fPIC" && make && \
  cp src/.libs/libunwind.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && wget https://ftpmirror.gnu.org/binutils/binutils-2.43.tar.gz && \
  tar -xvf binutils-2.43.tar.gz && \
  cd binutils-2.43/libiberty && \
  ./configure CFLAGS="-fPIC" && \
  make && \
  cp libiberty.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone https://github.com/gflags/gflags.git && cd gflags && \
  cmake . -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE -DBUILD_SHARED_LIBS=OFF && \
  make && \
  cp lib/libgflags.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone --depth 1 --branch 1.2.1 https://github.com/google/snappy.git && cd snappy && \
  git submodule update --init && \
  sed -i 's/-fno-rtti//g' CMakeLists.txt && \
  cmake . -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="-march=znver3 -mtune=znver3 -O3" \
    -DSNAPPY_BUILD_TESTS=OFF \
    -DSNAPPY_BUILD_BENCHMARKS=OFF && \
  make -j"$(nproc)" && \
  make install && ldconfig

RUN cd /opt && git clone --depth 1 --branch 20240722.0 https://github.com/abseil/abseil-cpp.git && cd abseil-cpp && \
  mkdir build && cd build && \
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="-march=znver3 -mtune=znver3 -O3" \
    -DABSL_BUILD_TESTING=OFF \
    -DABSL_PROPAGATE_CXX_STD=ON && \
  make -j"$(nproc)" && \
  make install && ldconfig

RUN cd /opt && git clone --depth 1 --branch 2025-11-05 https://github.com/google/re2.git && cd re2 && \
  cmake . -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="-march=znver3 -mtune=znver3 -O3" \
    -DBUILD_SHARED_LIBS=OFF \
    -DRE2_BUILD_TESTING=OFF && \
  make -j"$(nproc)" && \
  make install && ldconfig

# Copy source
WORKDIR /rocks-level
COPY . .

# Build libzstd using makefile in rocksdb
RUN cd deps/rocksdb/rocksdb && make libzstd.a && \
  cp libzstd.a /usr/lib/x86_64-linux-gnu/

# This will build rocksdb (deps/rocksdb/rocksdb.gyp)
RUN yarn --ignore-scripts

# This will build rocks-level bindings (binding.gyp)
RUN npx prebuildify -t 25.6.0 --napi --strip --arch x64

RUN yarn test-prebuild

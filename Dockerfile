FROM node:22.9.0

ENV CMAKE_BUILD_PARALLEL_LEVEL=32 MAKEFLAGS=-j32 JOBS=32 DEBUG_LEVEL=0

RUN apt update && apt install liburing-dev cmake -y

# Clone and build folly
RUN apt update && apt install sudo -y
RUN mkdir -p /opt/folly && cd /opt/folly && \
  git clone --depth 1 --branch v2024.11.25.00 https://github.com/facebook/folly . && \
  ./build/fbcode_builder/getdeps.py install-system-deps --recursive && \
  ./build/fbcode_builder/getdeps.py build --no-tests --extra-cmake-defines='{"CMAKE_CXX_FLAGS": "-fPIC"}'

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

RUN cd /opt && wget https://ftp.gnu.org/gnu/binutils/binutils-2.43.tar.gz && \
  tar -xvf binutils-2.43.tar.gz && \
  cd binutils-2.43/libiberty && \
  ./configure CFLAGS="-fPIC" && \
  make && \
  cp libiberty.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone https://github.com/gflags/gflags.git && cd gflags && \
  cmake . -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE -DBUILD_SHARED_LIBS=OFF && \
  make && \
  cp lib/libgflags.a /usr/lib/x86_64-linux-gnu/

RUN cd /opt && git clone https://github.com/jemalloc/jemalloc.git && cd jemalloc && \
  CFLAGS="-fPIC" ./autogen.sh && \
  make && \
  cp lib/libjemalloc.a /usr/lib/x86_64-linux-gnu/ && \
  cp -rv include/jemalloc /usr/include/

# RUN cd /opt && git clone https://github.com/abseil/abseil-cpp.git && cd abseil-cpp && \
#   mkdir build && cd build && \
#   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
#   make && \
#   make install

# RUN cd /opt && git clone https://github.com/google/re2.git && cd re2 && \
#   cmake . -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
#   make && \
#   cp libre2.a /usr/lib/x86_64-linux-gnu/ && \
#   mkdir -p /usr/include/re2 && cp re2/*.h /usr/include/re2/

# Copy source
WORKDIR /rocks-level
COPY . .

# Build libzstd using makefile in rocksdb
RUN cd deps/rocksdb/rocksdb && make libzstd.a && \
  cp libzstd.a /usr/lib/x86_64-linux-gnu/

# This will build rocksdb (deps/rocksdb/rocksdb.gyp)
RUN yarn --ignore-scripts

# This will build rocks-level bindings (binding.gyp)
RUN npx prebuildify -t 22.9.0 --napi --strip --arch x64

RUN yarn test-prebuild

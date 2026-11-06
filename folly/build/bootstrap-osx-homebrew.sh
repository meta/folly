#!/bin/bash -x
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The only prerequisite should be homebrew. If something doesn't work out of
# the box with just homebrew, let's fix it.

# fail fast
set -e

BUILD_DIR=${BUILD_DIR:-build}

# brew install alias
brew_install() {
    brew install "$@" || brew upgrade "$@"
}

# install deps
install_deps() {
    # folly deps
    dependencies=(
        boost
        cmake
        double-conversion
        gflags
        glog
        jemalloc
        libevent
        lz4
        openssl
        pkg-config
        snappy
        xz
        fmt
    )

    # fetch deps
    for dependency in "${dependencies[@]}"; do
        brew_install "${dependency}"
    done
}

install_deps

# Allows this script to be invoked from anywhere in the source tree but the
# BUILD_DIR we create will always be in the top level folly directory
TOP_LEVEL_DIR="$(cd "$(dirname -- "$0")"/../.. ; pwd)"  # folly
cd "$TOP_LEVEL_DIR"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

OPENSSL_INCLUDES=$(brew --prefix openssl)/include
cmake \
    -DOPENSSL_INCLUDE_DIR="${OPENSSL_INCLUDES}" \
    -DFOLLY_HAVE_WEAK_SYMBOLS=ON \
    "$@" \
    ..

# fetch googletest, if doesn't exist
GTEST_VER=1.10.0
GTEST_DIR=gtest-${GTEST_VER}
if [ ! -d ${GTEST_DIR} ]; then
    mkdir ${GTEST_DIR}
    curl -SL \
        https://github.com/google/googletest/archive/release-${GTEST_VER}.tar.gz | \
        tar -xvzf - --strip-components=1 -C ${GTEST_DIR}
fi

# make, test, install
make
make install

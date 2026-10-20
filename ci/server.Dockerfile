# The fork's server build on upstream's published images (the same ones
# upstream's CI uses; tags pinned from misc/github_env_linux at this commit),
# so the toolchain and the vcpkg cache are upstream's and nothing is rebuilt.
# Everything happens inside the build: configure, build, T0 when the master
# .esm files are present in skyrim_data_files/, and a runtime image carrying
# build/dist/server that sky-srv's compose runs.
#
# From the fork root:  docker build -f ci/server.Dockerfile -t skymp-server:parity .

ARG DEPS_IMAGE=skymp/skymp-vcpkg-deps:733f2d5
ARG RUNTIME_IMAGE=skymp/skymp-runtime-base:733f2d5
# vcpkg is a submodule that upstream's .dockerignore keeps out of the context;
# upstream's own builder stage clones it at the pinned commit, so does this one.
# The CI job passes both from .gitmodules and `git ls-tree HEAD vcpkg`.
ARG VCPKG_URL=https://github.com/microsoft/vcpkg.git
ARG VCPKG_COMMIT

FROM ${DEPS_IMAGE} AS skymp-parity-builder
# ARGs declared before FROM are visible only to FROM; redeclare inside the stage.
ARG VCPKG_URL
ARG VCPKG_COMMIT
WORKDIR /src
# WORKDIR created /src as root; the build user must own the directory itself
# (sed -i and the build tree write there), exactly as upstream's Dockerfile does.
USER root
RUN chown skymp:skymp /src
# ccache (thuum ADR-041's build levers): it keys on content, not times, so the
# shared runner checkout, whose times change with every branch switch, no longer
# makes ninja's rebuilds full ones; the cache lives in a BuildKit cache mount
# beside the build tree's.
RUN command -v ccache >/dev/null \
 || (apt-get update && apt-get install -y --no-install-recommends ccache && rm -rf /var/lib/apt/lists/*)
# Rust for skymp-wire (thuum ADR-019): corrosion runs cargo inside the CMake
# build, and corrosion_add_cxxbridge wants the cxxbridge CLI at exactly the
# cxx version the workspace locks. Installed under /opt/rust for the build
# user; the runtime image needs none of it (the bridge links statically into
# scam_native.node, the fakeclient is a static-std binary). Before the source
# is copied in, so the layer is cached across builds: behind COPY . . it was
# reinstalled on every change, 49 s a build (thuum ADR-040 item 4, server-build
# 8454).
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo PATH=/opt/rust/cargo/bin:$PATH
RUN (command -v curl >/dev/null || (apt-get update && apt-get install -y --no-install-recommends curl ca-certificates)) \
 && curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain stable --component clippy,rustfmt \
 && cargo install --locked cxxbridge-cmd --version 1.0.202 \
 && rm -rf /opt/rust/cargo/registry \
 && chown -R skymp:skymp /opt/rust
USER skymp
COPY --chown=skymp:skymp . .
RUN if [ ! -f vcpkg/scripts/buildsystems/vcpkg.cmake ]; then \
      test -n "$VCPKG_COMMIT" || { echo 'VCPKG_COMMIT build-arg is required'; exit 1; }; \
      rm -rf vcpkg && git init -q vcpkg \
      && git -C vcpkg fetch -q --depth 1 "$VCPKG_URL" "$VCPKG_COMMIT" \
      && git -C vcpkg checkout -q FETCH_HEAD; \
    fi
# The CMake build tree and cargo's registry are BuildKit cache mounts, kept by
# sky-ci's daemon between builds, so a build recompiles what changed instead of
# everything (configure and build were 342 s of server-build 8454's 468; thuum
# ADR-040 item 4). The runner reuses its working copy, but a branch switch
# changes the times of every file that differs, so the tree alone saved only the
# Rust layer (476 to 399 s, server-build on m2-ci-server-cache); ccache turns the
# recompiles the times force into hits. A cache mount is no part of the image:
# the dist is copied out of it for the runtime stage.
# Unit tests read their data directory from the UNIT_DATA_DIR CMake option
# (unit/TestUtils.cpp GetDataDir); the hash and dist-contents checks run only
# with CI=true in the environment (unit/EspmTest.cpp, unit/DistContentsTest.cpp).
# ccache's statistics are zeroed before and printed after, so the job log
# carries each build's hit rate.
# The cached tree keeps what is decided by content or redone at every build:
# CMake's configure state, vcpkg's installed ports, and (in its own mount)
# ccache's objects. It drops before each build what is decided by file times,
# since in a tree every branch shares the times do not follow the content:
# server-build 9362 (m2-train-29 d34053e0) recompiled the .cpp files that differ
# from parity's but none of their includers, and linked unit/unit against
# objects built from parity's MpActor.h. So ninja's log and deps go (an edge with
# no log entry is dirty, so every object is rebuilt from this commit's sources
# through ccache), with cargo's target directory (it decides by times too) and
# the dist (rebuilt whole, which also clears what a test server wrote into it:
# m2-formdesc-world's world/load-order.json, thuum ADR-043, server-build 9323;
# a test server's runtime files still belong in run_integration_test.cmake's
# step 5).
RUN --mount=type=cache,id=skymp-server-build,target=/src/build,mode=0777,sharing=locked \
    --mount=type=cache,id=skymp-cargo-registry,target=/opt/rust/cargo/registry,mode=0777,sharing=locked \
    --mount=type=cache,id=skymp-ccache,target=/opt/ccache,mode=0777,sharing=locked \
    chmod +x build.sh && sed -i 's/\r$//' build.sh \
 && export CCACHE_DIR=/opt/ccache CCACHE_MAXSIZE=4G CCACHE_BASEDIR=/src \
 && ccache -z \
 && rm -rf build/dist build/cargo && rm -f build/.ninja_log build/.ninja_deps \
 && ./build.sh --configure -DUNIT_DATA_DIR=/src/skyrim_data_files \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 && ./build.sh --build \
 && ccache -s \
 && rm -rf /src/dist-server && cp -a build/dist/server /src/dist-server
RUN --mount=type=cache,id=skymp-server-build,target=/src/build,mode=0777,sharing=locked \
    if [ -f skyrim_data_files/Skyrim.esm ]; then \
      cd build && CI=true ctest --verbose; \
    else echo "no skyrim_data_files/Skyrim.esm: ctest skipped"; fi

FROM ${RUNTIME_IMAGE} AS skymp-server
WORKDIR /srv/skymp
COPY --from=skymp-parity-builder --chown=skymp:skymp /src/dist-server /srv/skymp
# WORKDIR created /srv/skymp as root; the server dumps its settings into its cwd.
RUN chown skymp:skymp /srv/skymp
USER skymp
CMD ["sh", "-c", "ls /srv/skymp && echo 'entrypoint is set from launch_server in M0 Track S1'"]

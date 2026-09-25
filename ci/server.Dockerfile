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

FROM ${DEPS_IMAGE} AS skymp-parity-builder
WORKDIR /src
COPY --chown=skymp:skymp . .
# WORKDIR created /src as root; the build user must own the directory itself
# (sed -i and the build tree write there), exactly as upstream's Dockerfile does.
USER root
RUN chown skymp:skymp /src
USER skymp
# Unit tests read their data directory from the UNIT_DATA_DIR CMake option
# (unit/TestUtils.cpp GetDataDir); the hash and dist-contents checks run only
# with CI=true in the environment (unit/EspmTest.cpp, unit/DistContentsTest.cpp).
RUN chmod +x build.sh && sed -i 's/\r$//' build.sh \
 && ./build.sh --configure -DUNIT_DATA_DIR=/src/skyrim_data_files \
 && ./build.sh --build
RUN if [ -f skyrim_data_files/Skyrim.esm ]; then \
      cd build && CI=true ctest --verbose; \
    else echo "no skyrim_data_files/Skyrim.esm: ctest skipped"; fi

FROM ${RUNTIME_IMAGE} AS skymp-server
WORKDIR /srv/skymp
COPY --from=skymp-parity-builder --chown=skymp:skymp /src/build/dist/server /srv/skymp
USER skymp
CMD ["sh", "-c", "ls /srv/skymp && echo 'entrypoint is set from launch_server in M0 Track S1'"]

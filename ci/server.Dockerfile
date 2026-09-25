# The fork's server build, stacked on upstream's Dockerfile stages so the
# toolchain and the vcpkg cache are upstream's. Everything happens inside the
# build: configure, build, T0 (when master files are present), and a runtime
# image with build/dist/server that sky-srv's compose runs.
#
# Build from the fork root:
#   docker build -f ci/server.Dockerfile -t skymp-server:parity .
# Requires the skymp-vcpkg-deps stage from ./Dockerfile; docker builds it on
# demand through the FROM below.

FROM skymp-vcpkg-deps AS skymp-parity-builder
WORKDIR /src
COPY --chown=skymp:skymp . .
USER skymp
RUN chmod +x build.sh && sed -i 's/\r$//' build.sh \
 && ./build.sh --configure \
 && ./build.sh --build
# T0: ctest needs the master .esm files; skip loudly when they are absent.
RUN if [ -d skyrim_data_files ] && [ -f skyrim_data_files/Skyrim.esm ]; then \
      export SKYRIM_DIR=/src/skyrim_data_files && cd build && ctest --verbose; \
    else echo "no skyrim_data_files: ctest skipped"; fi

FROM skymp-runtime-base AS skymp-server
WORKDIR /srv/skymp
COPY --from=skymp-parity-builder --chown=skymp:skymp /src/build/dist/server /srv/skymp
USER skymp
CMD ["sh", "-c", "ls /srv/skymp && echo 'set the entrypoint from launch_server (M0, Track S1)'"]

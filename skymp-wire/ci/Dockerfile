# Rust CI image for skymp-wire (ADR-015, ADR-016). Built by the fork's
# pipeline on the CI host's own Docker daemon; later jobs use it as their image.
# Stable for the crates, nightly for cargo-fuzz, the three CLIs the gates need.
# The base comes from the registry the pipeline names (BASE_REGISTRY, from the
# project's SKY_REGISTRY variable), a copy of Docker Hub's rust:1-bookworm, so a
# pipeline does not depend on the outside world's DNS; with no registry named it
# is Docker Hub's own library image, so a pipeline still builds. To move to a
# newer Rust: docker pull rust:1-bookworm, docker tag it
# <registry>/rust:1-bookworm, docker push.
# The base names its toolchain by version (rustup-init --default-toolchain
# 1.xx.y), and skymp-wire/rust-toolchain.toml asks for the channel "stable",
# so every job had rustup install "stable" at its start, a full toolchain
# download through the outside world's DNS (2026-10-23: the estate's resolver
# down, wire-test and difftest-build red in 25 s at channel-rust-stable.toml,
# the nightly jobs green since nightly is baked). The channel is installed here
# by name, with the components the toolchain file lists, and made the default,
# so a job never reaches for static.rust-lang.org; the base's version-named
# toolchain goes, the image carrying one stable and one nightly.
ARG BASE_REGISTRY=docker.io/library
FROM ${BASE_REGISTRY}/rust:1-bookworm
RUN old="$(rustup default | cut -d' ' -f1)" \
 && rustup toolchain install stable --component clippy --component rustfmt \
 && rustup default stable \
 && rustup toolchain uninstall "$old" \
 && rustup toolchain install nightly --profile minimal \
 && cargo install --locked cargo-deny cargo-fuzz cbindgen \
 && rm -rf /usr/local/cargo/registry

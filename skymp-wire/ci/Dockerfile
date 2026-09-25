# Rust CI image for skymp-wire (ADR-015, ADR-016). Built by the fork's
# pipeline on sky-ci's own Docker daemon; later jobs use it as their image.
# Stable for the crates, nightly for cargo-fuzz, the three CLIs the gates need.
FROM rust:1-bookworm
RUN rustup component add clippy rustfmt \
 && rustup toolchain install nightly --profile minimal \
 && cargo install --locked cargo-deny cargo-fuzz cbindgen \
 && rm -rf /usr/local/cargo/registry

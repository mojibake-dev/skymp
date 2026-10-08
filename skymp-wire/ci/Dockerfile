# Rust CI image for skymp-wire (ADR-015, ADR-016). Built by the fork's
# pipeline on sky-ci's own Docker daemon; later jobs use it as their image.
# Stable for the crates, nightly for cargo-fuzz, the three CLIs the gates need.
# The base comes from the estate's registry on sky-ci, not Docker Hub, so a
# pipeline does not depend on the outside world's DNS (2026-10-08: four hours
# without it failed every pipeline at this line). Docker Hub's rust:1-bookworm
# was copied there on 2026-10-08 (image created 2026-10-06T04:43:49Z); to move
# to a newer Rust, on sky-ci: docker pull rust:1-bookworm, docker tag it
# 10.10.70.12:5000/rust:1-bookworm, docker push.
FROM 10.10.70.12:5000/rust:1-bookworm
RUN rustup component add clippy rustfmt \
 && rustup toolchain install nightly --profile minimal \
 && cargo install --locked cargo-deny cargo-fuzz cbindgen \
 && rm -rf /usr/local/cargo/registry

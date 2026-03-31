# ──────────────────────────────────────────────────────────────────────────────
# MLB Live Scores ACAP — Dockerfile
#
# Builds using the official Axis ACAP Native SDK image.
# Target: Axis C1720 (aarch64 / ARTPEC-8)
#
# Usage:
#   docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.0 .
#   docker cp $(docker create mlb_scores:1.0):/opt/app ./build
#   # → ./build/mlb_scores_1_0_0_aarch64.eap
# ──────────────────────────────────────────────────────────────────────────────

ARG ARCH=aarch64
ARG VERSION=12.9.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Copy application source into the SDK build directory
COPY ./app /opt/app/

WORKDIR /opt/app

# Source the SDK environment and build
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

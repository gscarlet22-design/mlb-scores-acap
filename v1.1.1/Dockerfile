# ──────────────────────────────────────────────────────────────────────────────
# MLB Live Scores ACAP — Dockerfile
#
# Builds using the official Axis ACAP Native SDK image.
# Target: Axis C1720 (aarch64 / ARTPEC-8)
#
# Usage:
#   docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.1.1 .
#   docker cp $(docker create mlb_scores:1.1.1):/opt/app ./build
#   # → ./build/mlb_scores_1_1_1_aarch64.eap
# ──────────────────────────────────────────────────────────────────────────────

ARG ARCH=aarch64
ARG VERSION=12.9.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Download cJSON source (not bundled in Axis SDK sysroot)
# Install ffmpeg for audio conversion (MP3 → µ-law .au)
RUN apt-get update -qq && apt-get install -y --no-install-recommends curl ca-certificates ffmpeg && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h -o /tmp/cJSON.h && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c -o /tmp/cJSON.c

# Copy application source into the SDK build directory
COPY ./app /opt/app/

# Place cJSON alongside the app source
RUN cp /tmp/cJSON.h /opt/app/cJSON.h && \
    cp /tmp/cJSON.c /opt/app/cJSON.c

# Convert bundled MP3 clips to 8 kHz mono µ-law .au
# transmit.cgi requires audio/basic (G.711 µ-law 8 kHz mono),
# and mediaclip?action=download returns files in their uploaded format,
# so uploading as .au means downloading as .au — matching our volume pipeline.
RUN ffmpeg -y -i /opt/app/audio/hit_a_run.mp3 \
        -ar 8000 -ac 1 -acodec pcm_mulaw -f au \
        /opt/app/audio/hit_a_run.au && \
    ffmpeg -y -i /opt/app/audio/inning_change_organ.mp3 \
        -ar 8000 -ac 1 -acodec pcm_mulaw -f au \
        /opt/app/audio/inning_change_organ.au

WORKDIR /opt/app

# Source the SDK environment and build
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

WORKDIR /opt/app

# Source the SDK environment and build
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

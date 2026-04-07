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
# I hope this works! 
# ──────────────────────────────────────────────────────────────────────────────

ARG ARCH=aarch64
ARG VERSION=12.9.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Download cJSON and minimp3 (neither is bundled in the Axis SDK sysroot).
# minimp3 is a single-header pure-C MP3 decoder; no new LDLIBS required.
RUN apt-get update -qq && apt-get install -y --no-install-recommends curl ca-certificates && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h \
         -o /tmp/cJSON.h && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c \
         -o /tmp/cJSON.c && \
    curl -sL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h \
         -o /tmp/minimp3.h && \
    curl -sL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h \
         -o /tmp/minimp3_ex.h

# Copy application source into the SDK build directory
COPY ./app /opt/app/

# Place cJSON and minimp3 alongside the app source
RUN cp /tmp/cJSON.h      /opt/app/cJSON.h      && \
    cp /tmp/cJSON.c      /opt/app/cJSON.c      && \
    cp /tmp/minimp3.h    /opt/app/minimp3.h    && \
    cp /tmp/minimp3_ex.h /opt/app/minimp3_ex.h

WORKDIR /opt/app

# Source the SDK environment and build
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

# Inject audio files into the .eap package.
# schema 1.9.0 has no "files" manifest key, so acap-build won't bundle
# the audio/ directory automatically.  The .eap is a gzipped tar archive,
# so we unpack it, add audio/, and repack.  The device installs the archive
# contents to /usr/local/packages/mlb_scores/, putting the .au files at
# /usr/local/packages/mlb_scores/audio/ where the app expects them.
RUN set -e && \
    EAP=$(ls *.eap) && \
    mkdir -p /tmp/eap_work && \
    tar -xzf "$EAP" -C /tmp/eap_work && \
    mkdir -p /tmp/eap_work/audio && \
    cp audio/hit_a_run.mp3           /tmp/eap_work/audio/ && \
    cp audio/inning_change_organ.mp3 /tmp/eap_work/audio/ && \
    tar -czf "$EAP" -C /tmp/eap_work . && \
    rm -rf /tmp/eap_work

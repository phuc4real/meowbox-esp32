# MeowBox firmware build image: ESP-IDF from the release/v6.1 branch, pinned at the commit the releases are built
# with. Not the v6.1.0 / v6.1.1 tags: they leave ~12 KB internal RAM free when idle instead of ~48 KB (CLAUDE.md).
# Built and run by idf.ps1 / idf.sh next to this file.
ARG IDF_IMAGE=espressif/idf:release-v6.1
FROM ${IDF_IMAGE}

# Change IDF_REF (full hash, a commit on release/v6.1) to move to a newer IDF; the image is rebuilt on the next run.
ARG IDF_REF=c43d665a96c0d90ea4740fa3ba12aaf89e48e568

RUN set -eux; \
    git config --system --add safe.directory '*'; \
    cd "$IDF_PATH"; \
    git fetch --depth 1 origin "$IDF_REF"; \
    git -c advice.detachedHead=false checkout "$IDF_REF"; \
    git submodule update --init --recursive --depth 1; \
    ./install.sh esp32s3; \
    git -C "$IDF_PATH" rev-parse HEAD

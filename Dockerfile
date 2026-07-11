# syntax=docker/dockerfile:1.7

FROM debian:trixie-slim@sha256:28de0877c2189802884ccd20f15ee41c203573bd87bb6b883f5f46362d24c5c2 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG WIMFORGE_VERSION=0.1.0

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
        python3 \
        qt6-base-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S /src -B /build -G Ninja \
        -DBUILD_TESTING=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DWIMFORGE_BUILD_VERSION="${WIMFORGE_VERSION}" \
        -DWIMFORGE_CLI_ONLY=ON \
    && cmake --build /build --parallel

FROM build AS test

RUN ctest --test-dir /build --output-on-failure \
        --label-exclude windows-powershell

FROM debian:trixie-slim@sha256:28de0877c2189802884ccd20f15ee41c203573bd87bb6b883f5f46362d24c5c2 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        libqt6core6t64 \
        python3 \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 65532 wimforge \
    && useradd --uid 65532 --gid 65532 --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin wimforge \
    && install --directory --owner=65532 --group=65532 --mode=0750 \
        /app /config /profiles

COPY --from=test --chown=65532:65532 /build/WimForgeCli /usr/local/bin/WimForgeCli
COPY --chown=65532:65532 server/provisioning_server.py /app/provisioning_server.py
COPY LICENSE /usr/share/licenses/wimforge/LICENSE

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    WIMFORGE_CLI=/usr/local/bin/WimForgeCli \
    WIMFORGE_CONFIG_FILE=/config/provisioning.json \
    WIMFORGE_PROFILE_DIR=/profiles \
    WIMFORGE_LISTEN=0.0.0.0 \
    WIMFORGE_PORT=8080

USER 65532:65532
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["python3", "-c", "import os, urllib.request; port=os.environ.get('WIMFORGE_PORT','8080'); urllib.request.urlopen(f'http://127.0.0.1:{port}/healthz', timeout=3).read()"]

ENTRYPOINT ["/usr/bin/tini", "--", "python3", "/app/provisioning_server.py"]
CMD ["serve"]

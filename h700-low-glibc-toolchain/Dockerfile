ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

ARG APT_MIRROR=

RUN if [ -n "$APT_MIRROR" ]; then \
      sed -i "s|http://archive.ubuntu.com/ubuntu|$APT_MIRROR|g; s|http://security.ubuntu.com/ubuntu|$APT_MIRROR|g" /etc/apt/sources.list; \
    fi \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      file \
      g++-aarch64-linux-gnu \
      pkg-config \
      python3 \
      unzip \
      zip \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work

CMD ["/bin/bash"]

FROM martenseemann/quic-network-simulator-endpoint:latest AS builder

ARG TARGETPLATFORM
RUN echo "TARGETPLATFORM: ${TARGETPLATFORM}"

RUN apt-get update && apt-get install -y git make autoconf automake libtool pkg-config gnutls-dev kmod linux-headers-$(uname -r)

RUN git clone --recursive --shallow-submodules --depth 1 https://github.com/ngtcp2/nghttp3 && \
    cd nghttp3 && \
    autoreconf -i && \
    ./configure --disable-dependency-tracking --enable-lib-only && \
    make -j$(nproc) && make install && cd .. && rm -rf nghttp3

# build other branches / commits / tags using --build-arg GITREF="<git reference>"
ARG GITREF="interop"

# XXX: interop wrong, moritzbuhl wrong
RUN git clone https://github.com/moritzbuhl/quic && \
    cd quic && \
    git checkout ${GITREF} && \
    ./autogen.sh && \
    mkdir -p /lib/modules/`uname -r`/kernel/net/quic && \
    install -m 644 modules/include/uapi/linux/quic.h /usr/include/linux && \
    ./configure --prefix=/usr && \
    make && \
    make install && \
    cd tests && \
    make interop_test

FROM martenseemann/quic-network-simulator-endpoint:latest

WORKDIR /quic
COPY --from=builder /quic/tests/interop_test /quic/tests/run_endpoint.sh ./
RUN chmod +x run_endpoint.sh

ENTRYPOINT [ "./run_endpoint.sh" ]

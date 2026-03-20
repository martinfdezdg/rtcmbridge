FROM ubuntu:22.04 as builder_ntrip

RUN apt-get update && \
    apt-get install -y libssl-dev make gcc && \
    rm -rf /var/lib/apt/lists/*

ADD ntripcaster-2.0.48.tar.bz2 .

RUN ./ntripcaster-2.0.48/configure --prefix /usr/local/ntripcaster \
    && make \
    && make install \
    && rm -rf ./ntripcaster-2.0.48 \
    && rm -f Makefile config.h config.status stamp-h1 \
    && sed -i 's#$BINDIR/casterwatch $SUB &#$BINDIR/casterwatch $SUB#g' /usr/local/ntripcaster/bin/ntripcaster


FROM ubuntu:22.04

RUN mkdir -p /usr/local/ntripcaster
COPY --from=builder_ntrip /usr/local/ntripcaster/ /usr/local/ntripcaster/

WORKDIR /usr/local/ntripcaster

RUN apt-get update && \
    apt-get install -y less vim openssl && \
    rm -rf /var/lib/apt/lists/*

EXPOSE 80
EXPOSE 2101

ENTRYPOINT ["/usr/local/ntripcaster/bin/ntripcaster","start"]

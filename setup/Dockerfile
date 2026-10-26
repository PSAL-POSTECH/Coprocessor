# The environment image: the RISC-V toolchain, pk, the pinned Spike and gem5, and Verilator; built by setup/image.sh.
ARG BASE_IMAGE

FROM ${BASE_IMAGE} AS system
ARG COPROCESSOR_ENV_ROOT
ENV COPROCESSOR_ENV_ROOT=${COPROCESSOR_ENV_ROOT}
COPY versions.env system.sh /opt/coprocessor-setup/
RUN /opt/coprocessor-setup/system.sh

FROM system AS build
ARG JOBS=4
COPY setup.sh /opt/coprocessor-setup/
RUN /opt/coprocessor-setup/setup.sh -j "$JOBS" toolchain pk spike gem5 verilator

FROM build AS slim
COPY slim.sh /opt/coprocessor-setup/
RUN /opt/coprocessor-setup/slim.sh

FROM system
COPY --from=slim ${COPROCESSOR_ENV_ROOT} ${COPROCESSOR_ENV_ROOT}
LABEL org.opencontainers.image.source=https://github.com/PSAL-POSTECH/Coprocessor
LABEL org.opencontainers.image.description="Build and run environment for Coprocessor: RISC-V toolchain, pk, Spike, gem5 and Verilator"

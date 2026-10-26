# The environment image: the RISC-V toolchain, pk, the pinned Spike and gem5, and Verilator; built by setup/image.sh.
ARG BASE_IMAGE

FROM ${BASE_IMAGE} AS system
ARG VCIX_ENV_ROOT
ENV VCIX_ENV_ROOT=${VCIX_ENV_ROOT}
COPY versions.env system.sh /opt/vcix-setup/
RUN /opt/vcix-setup/system.sh

FROM system AS build
ARG JOBS=4
COPY setup.sh /opt/vcix-setup/
RUN /opt/vcix-setup/setup.sh -j "$JOBS" toolchain pk spike gem5 verilator

FROM build AS slim
COPY slim.sh /opt/vcix-setup/
RUN /opt/vcix-setup/slim.sh

FROM system
COPY --from=slim ${VCIX_ENV_ROOT} ${VCIX_ENV_ROOT}
LABEL org.opencontainers.image.source=https://github.com/PSAL-POSTECH/Coprocessor
LABEL org.opencontainers.image.description="Build and run environment for Coprocessor: RISC-V toolchain, pk, Spike, gem5 and Verilator"

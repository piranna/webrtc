FROM ubuntu:focal-20210325

# Install pkg dependencies
ENV DEBIAN_FRONTEND=noninteractive
RUN apt update
RUN apt install -y git 
RUN apt install -y curl 
RUN apt install -y wget 
RUN apt install -y lsb-core

# Download 3rd dependencies
WORKDIR /dependencies
RUN git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
RUN mv depot_tools /usr/bin

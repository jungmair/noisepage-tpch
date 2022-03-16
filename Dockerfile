FROM ubuntu:20.04
CMD bash

# Install Ubuntu packages.
# Please add packages in alphabetical order.
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update 
RUN apt-get -y install sudo ninja-build
COPY script/installation/packages.sh install-script.sh 
RUN echo y | ./install-script.sh all
RUN git clone https://github.com/malin1993ml/tpl_tables.git /tpl_tables
WORKDIR /tpl_tables
RUN apt-get -y install python-is-python3
RUN bash gen_tpch.sh 1
COPY . /repo
RUN mkdir /repo/build
WORKDIR /repo/build
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -NOISEPAGE_BUILD_BENCHMARKS=ON -DNOISEPAGE_USE_JEMALLOC=ON -DNOISEPAGE_UNITY_BUILD=ON ..
RUN ninja noisepage



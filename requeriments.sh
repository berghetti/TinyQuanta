#!/bin/bash

# Ubuntu
VERSION=$(grep -oP '(?<=VERSION_ID=")\d+' /etc/os-release)
if [[ $VERSION -ge 18 ]]; then

  wget -qO - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -

  # focal is last version with clang-12 available
  echo "deb http://apt.llvm.org/bionic/ llvm-toolchain-bionic-12 main" \
  | sudo tee /etc/apt/sources.list.d/llvm-2.list

  wget http://es.archive.ubuntu.com/ubuntu/pool/main/libf/libffi/libffi7_3.3-4_amd64.deb
  sudo dpkg -i libffi7_3.3-4_amd64.deb

  sudo apt update
  sudo apt install clang-12 -y

fi

sudo apt install libboost-coroutine-dev -y

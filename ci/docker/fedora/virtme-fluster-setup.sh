#! /bin/bash

set -eux

sudo dnf install -y bc

# Install virtme-ng
pushd /tmp/
git clone https://github.com/arighi/virtme-ng.git
pushd virtme-ng
git fetch --tags
git checkout v1.8
sudo ./setup.py install --prefix=/usr
popd
popd

# Install fluster
pushd /opt/
sudo mkdir ./fluster
sudo chown containeruser:containeruser ./fluster/

git clone  https://github.com/fluendo/fluster.git ./fluster
pushd fluster
git checkout 303a6edfda1701c8bc351909fb1173a0958810c2
./fluster.py download
popd
popd

# Build a linux image for virtme fluster tests
bash ./ci/scripts/build-linux.sh \
    "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" \
    "v6.6.93" \
    /opt/linux/bzImage \
    'MEDIA_SUPPORT' \
    'MEDIA_TEST_SUPPORT' \
    'V4L_TEST_DRIVERS' \
    'CONFIG_VIDEO_VISL'

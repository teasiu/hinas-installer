<!-- SPDX-FileCopyrightText: no
     SPDX-License-Identifier: CC0-1.0
-->

# Calamares: Distribution-Independent Installer Framework
---------

```
ubuntu 24.04
sudo apt update && sudo apt install -y git build-essential cmake qtbase5-dev qttools5-dev-tools python3 python3-pip libkf5config-dev libkf5coreaddons-dev libkf5i18n-dev libkf5xmlgui-dev extra-cmake-modules curl libgl1-mesa-dev squashfs-tools genisoimage rsync

sudo apt install libkf5declarative-dev
sudo apt install libkpmcore-dev
sudo apt install python3-pyqt5 python3-yaml

rm -rf *
mkdir build && cd build
cmake .. -DPLUGIN_INSTALL=ON -DUSE_POLKIT=ON -DALLOW_FREEBSD_PARTITIONING=OFF

sudo make
sudo make install

```

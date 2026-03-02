# Proxmark3GUI

基于wh201906/Proxmark3GUI大佬的项目修改的Pm3的GUI，原项目可在Windows/Linux/macOS系统下运行，当前修改的只在mac apple芯片的mac上验证过，其它系统不知道支持情况。[冰人版](https://github.com/RfidResearchGroup/proxmark3)客户端和固件  

以下内容是原项目的，只更新了修改后的ui图片.
***

## 功能

+ 快速查找可用端口并连接
+ 支持直接输入PM3命令(官方版/冰人版)
+ 有针对于Mifare卡（IC卡）的图形界面
    + 支持不同大小的卡片(MINI, 1K, 2K, 4K)
    + 支持编辑Mifare扇区数据
    + 支持读取全卡/选中扇区，读卡逻辑更加智能
    + 支持写入全卡/选中扇区
    + 可以打开二进制/文本格式的扇区数据文件
    + 分析访问控制位（Access Bits）
    + 支持UID卡操作（UID快速读写，UFUID锁卡）
+ 支持部分低频命令
+ 自定义UI界面，各选项卡可拆分组合
+ ...

***

## 预览图
<img width="720" height="565" alt="image" src="https://github.com/user-attachments/assets/252c38e7-9551-4fd6-84a0-8d1652b5d6e4" />
<img width="720" height="565" alt="image" src="https://github.com/user-attachments/assets/15fdb75c-70d8-4d43-ae16-6701663ad735" />
<img width="720" height="565" alt="image" src="https://github.com/user-attachments/assets/0fbaeadd-01e2-4fdb-8841-24f738fdc06b" />
<img width="720" height="565" alt="image" src="https://github.com/user-attachments/assets/1cfde9e8-16f0-400a-b9ba-9bde8bf92624" />
<img width="720" height="565" alt="image" src="https://github.com/user-attachments/assets/bf2ac742-6374-45ca-a754-8e3422de8f66" />

***

## 关于预编译Windows客户端

一位国外大佬 [Gator96100](https://github.com/Gator96100) 做了个 [ProxSpace](https://github.com/Gator96100/ProxSpace) 以便在Windows平台上编译PM3固件和客户端，他还把自己编译好的客户端放到了[网站](https://www.proxmarkbuilds.org/)上  
release页面中有含客户端的GUI。这个GUI也可以搭配你自己的客户端使用  
(本来打算在CSDN下载里面放几个最新版客户端的，结果不能把下载币改为0)  
感谢大佬  

***

## 下载Windows版本
[release](https://github.com/wh201906/Proxmark3GUI/releases) 页面当中包含了编译好的Windows程序，解压后即可使用  
`Vx.x.x-win64.7z`是不带客户端的纯GUI程序，可配合已有的客户端使用  
`Vx.x.x-win64-xxxxxxx.7z`包含了对应的客户端，打开`Vx.x.x-win64-xxxxxxx\GUI\Proxmark3GUI.exe`即可使用  

SourceForge平台上也可下载  
[![Download Proxmark3GUI](https://a.fsdn.com/con/app/sf-download-button)](https://sourceforge.net/projects/proxmark3gui/files/latest/download)  

## 在Linux系统下编译
```
cd ~
sudo apt-get update
sudo apt-get install git build-essential
sudo apt-get install qt5-default libqt5serialport5-dev 
git clone https://github.com/wh201906/Proxmark3GUI.git --depth=1
cd Proxmark3GUI
mkdir build && cd build
qmake ../src
make -j4 && make clean
./Proxmark3GUI
```

## 在macOS系统下编译
```
cd ~
brew update
brew install qt@5
brew link qt5 --force
git clone https://github.com/wh201906/Proxmark3GUI.git --depth=1
cd Proxmark3GUI
mkdir build && cd build
qmake ../src
make -j4 && make clean
open Proxmark3GUI.app
```

> 为使GUI在macOS下正确连接设备，需要更改一些设置

![macOS_settings](macOS_settings.png)

***
## 教程
[1.快速上手](../tutorial/Quickstart/quickstart_zh_CN.md)  
[2.使用ProxSpace编译的客户端](../tutorial/Work_With_ProxSpace/work_with_proxspace_zh_CN.md)  
[3.编辑Mifare(IC)卡数据](../tutorial/Edit_Mifare_Classic_data/Edit_Mifare_Classic_data_zh_CN.md)（无需PM3硬件）  
***

## 更新日志
[更新日志](../CHANGELOG/CHANGELOG_zh_CN.md)

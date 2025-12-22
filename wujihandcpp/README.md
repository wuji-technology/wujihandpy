# WujihandCpp: A Lightweight C++ SDK for Wujihand

这是一个使用 C++ **全新编写的** 舞肌灵巧手 SDK (Software Development Kit)。

提供更简洁、更高效、更易用的接口与灵巧手设备进行交互。

Python 版本 [WujihandPy](https://github.com/wuji-technology/wujihandpy)（基于 pybind11 绑定），已发布至 PyPI，可通过 pip 安装。

## 最低系统要求 (Linux)

最低系统要求仅适用于在本地使用 SDK 发布包的情况。
发布包头文件仅依赖 C++11，且在尽量旧的 glibc 版本下构建，可保证低版本发行版和编译器的兼容性。

若通过 Docker 进行开发，可在任意 Linux 发行版上使用，无需关注系统版本。

若通过源码编译安装，则必须使用完整支持 C++20 的编译器（GCC 13+/Clang 17+）进行构建。

### glibc 2.28+

使用 glibc 2.28 或更高版本的 Linux 发行版：
- Debian 10+
- Ubuntu 18.10+
- Fedora 29+
- CentOS/RHEL 8+

### C++11 编译工具链

支持 C++11 或更高版本的 C++ 编译器：
- GCC 5+
- Clang 4+
<!-- - MSVC 2015+ -->

### libusb 1.0

需安装 libusb 1.0 开发包：

- Debian/Ubuntu: `sudo apt install libusb-1.0-0-dev`
- Fedora/CentOS/RHEL: `sudo dnf install libusbx-devel`

## 最低系统要求 (Windows)

WujihandCpp 目前暂不支持 Windows，我们会尽快推进相关支持。

## 安装

### Docker (推荐)

我们提供了一个基于 Ubuntu 24.04 & GCC-14 的 Docker 镜像，内置了所有必要的依赖和编译工具链，见 [Docker 开发指南](docs/zh-cn/docker-develop-guide.md)。

SDK 已在镜像中全局安装，可直接在容器内进行开发。

Ubuntu 24 拥有较新的 glibc，GCC-14 支持完整的 C++20 特性，两者组合可确保 SDK 的最佳优化和使用体验。

### SDK 发布包​​

如果不希望使用 Docker，也可通过 [Release 页面](https://github.com/wuji-technology/wujihandcpp/releases) 的发布包​​进行安装。

- Debian/Ubuntu: `sudo apt install ./wujihandcpp-<version>-<arch>.deb`

- Fedora/CentOS/RHEL: `sudo dnf install ./wujihandcpp-<version>-<arch>.rpm`

- 其他发行版：可由 `wujihandcpp-<version>-<arch>.zip` 手动安装头文件和库文件。

### 从源码构建

见 (TODO)。

## 使用

链接 wujihandcpp 库即可。

### CMake

<!-- ```cmake
find_package(wujihandcpp REQUIRED)
target_link_libraries(your_target PRIVATE wujihandcpp::wujihandcpp)
``` -->

```cmake
target_link_libraries(<your_target> PRIVATE wujihandcpp)
```

### Make

```makefile
LDFLAGS += -lwujihandcpp
```

可在 `example` 目录中查看使用示例。

## 部分参考 API

### 引入头文件

```cpp
#include <wujihandcpp/data/data.hpp>   // For data types
#include <wujihandcpp/device/hand.hpp> // For hand device
```

### 连接至灵巧手

```cpp
wujihandcpp::device::Hand hand{usb_vid, usb_pid};
```

定义一个 `Hand` 对象，并传入其 USB VID 和 PID 即可连接。

在目前的固件实现中，所有灵巧手的 VID 固定为 `0x0483`，PID 固定为 `0x7530`：

```cpp
wujihandcpp::device::Hand hand{0x0483, 0x7530};
```

### 读数据

```cpp
read<typename Data>() -> Data::ValueType;
read<typename Data>() -> void; // For bulk-read
```

所有可使用的数据类型均定义在 `wujihandcpp/data/data.hpp` 中。

例如，读取灵巧手的上电运行时间(us)：

```cpp
uint32_t time = hand.read<wujihandcpp::data::hand::SystemTime>();
```

除整手唯一的数据外，每个关节也有自己的数据，定义在 `data::joint` 命名空间下。

例如，读取第1个手指（食指），第0个关节的当前位置数据：

```cpp
int32_t position = hand.finger(1).joint(0).read<wujihandcpp::data::joint::Position>();
```

用一条指令读取多个数据也是可行的，这被称为**批量读 (Bulk-Read)**。

例如，以下指令读取整手所有（20个）关节的当前位置数据：

```cpp
hand.read<wujihandcpp::data::joint::Position>();
```

由于一次性获取了多个数据，为避免不必要的开销，当进行批量读时，`read` 函数的返回值为 `void`。

此时若希望获取读取完成的数据，需在 `read` 后调用 `get` 函数：

```cpp
hand.finger(i).joint(j).get<wujihandcpp::data::joint::Position>();
```

`read` 函数会阻塞，直到读取完成。保证当函数返回时，读取一定成功。

不同于 `read` 函数，`get` 从不阻塞，它总是立即返回最后一次读取的数据。
若从未请求过数据，`get` 函数的返回值是未定义的。

### 写数据

写数据拥有类似的API，但多了一个参数用于传递目标值：

```cpp
write<typename Data>(Data::ValueType value) -> void;
```

例如，写入单个关节的目标位置数据：

```cpp
hand.finger(1).joint(0).write<wujihandcpp::data::joint::ControlPosition>(0x8FFFFF);
```

**批量写**数据也是可行的，例如，批量写入第1个手指的目标位置数据：

```cpp
hand.finger(1).write<wujihandcpp::data::joint::ControlPosition>(0x8FFFFF);
```

`write` 函数会阻塞，直到写入完成。保证当函数返回时，写入一定成功。

## 许可证

本项目采用 MIT 许可证，详情见 [LICENSE](LICENSE) 文件。
# wujihandpy

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![Release](https://img.shields.io/github/v/release/wuji-technology/wujihandpy)](https://github.com/wuji-technology/wujihandpy/releases)

Wuji Hand SDK: C++ core with Python bindings, for controlling and communicating with Wuji Hand. WujihandPy is the Python binding of [WujihandCpp](wujihandcpp/README.md), providing an easy-to-use Python API for Wujihand dexterous-hand devices. Supports synchronous, asynchronous, unchecked operations and real-time control.

📖 **Documentation**: [Quick Start](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/introduction) | [API Reference](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/api-reference)

## Appendix

### Performance and Optimization

While ensuring usability, WujihandPy has been optimized for performance and efficiency as much as possible.

We recommend prioritizing bulk read/write to maximize performance.

For scenarios that require smooth joint position control, be sure to use `realtime_controller`.

### References

- **Documentation**: [Quick Start](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/introduction/)
- **API Reference**: [API Reference](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/api-reference/)
- **URDF Files**: [wuji-hand-description](https://github.com/wuji-technology/wuji-hand-description)

## Contact

For any questions, please contact [support@wuji.tech](mailto:support@wuji.tech).

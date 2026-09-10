<p align="center">
  <img src="logo_neon.png" alt="VLSS5 Logo" width="220" />
</p>

<h1 align="center">VLSS5</h1>

<p align="center">
  <strong>High-Performance Native NVIDIA DLSS & Neural Reconstruction Game Overlay</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B17-00FF3C?style=flat-square&logo=c%2B%2B" alt="C++17" />
  <img src="https://img.shields.io/badge/DirectX-11%20%2F%2012-00FF3C?style=flat-square" alt="DirectX" />
  <img src="https://img.shields.io/badge/NVIDIA-DLSS%20%26%20NGX-76B900?style=flat-square&logo=nvidia" alt="NVIDIA DLSS" />
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-blue?style=flat-square&logo=windows" alt="Windows x64" />
  <img src="https://img.shields.io/badge/License-MIT-00FF3C?style=flat-square" alt="MIT License" />
</p>

---

## ⚡ Overview

**VLSS5** is a standalone, non-intrusive Windows overlay that applies **native NVIDIA DLSS Super Resolution** and **Neural Radiance / Ray Reconstruction (DLSS-NR)** to windowed or borderless games.

Unlike conventional injectors or memory hooks, VLSS5 utilizes the native **Windows Graphics Capture API** with high-throughput DirectX 11/12 GPU interop. Because it captures and renders externally without injecting code or modifying game binaries, it provides maximum compatibility and eliminates anti-cheat interference.

---

## 🌟 Key Features

- **🛡️ Zero-Injection Architecture**: External capture via DirectX/WinRT ensures no game files or memory spaces are modified.
- **🧠 Native NVIDIA NGX Integration**:
  - **DLSS Super Resolution**: Temporal upscaling powered by NVIDIA Tensor Cores.
  - **DLSS-NR (Neural Reconstruction)**: Advanced ray radiance reconstruction pass (Feature 18) for superior detail and stability.
- **🌊 Hardware-Accelerated Optical Flow**: Dedicated Direct3D 11 Compute Shader calculates real-time motion vectors with a hierarchical search window.
- **🎯 Dynamic UI Reactive Mask**: Automatically isolates static HUD elements and text to eliminate ghosting and motion smearing.
- **🌑 Deep Shadow Stabilization**: Special shadow-filtering algorithm suppresses low-luminance temporal noise and shadow pulsation in dark areas.
- **✨ Custom Neural Image Tuning**:
  - Real-time **Sharpness Control** (CAS / Neural unsharp filter).
  - Fine-grained intensity, structural contrast, tone balance, and resolution scale adjustment.
- **⚡ High-Priority GPU Pipeline**:
  - Thread priority boosted via `IDXGIDevice2::SetGPUThreadPriority(5)`.
  - Process priority set to `HIGH_PRIORITY_CLASS` with OS background frame-rate throttling disabled.
- **🎮 Modern Cyber Neon UI**: Responsive, dark-themed user interface in electric neon green with global hotkey rebinding and status tracking.

---

## ⌨️ Default Keyboard Shortcuts

| Shortcut | Action | Description |
| :---: | :--- | :--- |
| **`Alt + S`** | **Capture Toggle** | Starts or stops the overlay on the selected game window *(Rebindable in UI)* |
| **`F8`** | **Re-align Overlay** | Re-synchronizes overlay position and size to match the target window |
| **`F9`** | **FPS Toggle** | Toggles the real-time frame counter display on/off |
| **`F10`** | **DLSS Toggle** | Toggles DLSS Neural Upscaling on/off instantly for A/B comparison |

---

## 🖥️ System Requirements

- **Operating System**: Windows 10 (version 20H1 or newer) / Windows 11 (64-bit)
- **GPU**: NVIDIA GeForce RTX Series (RTX 20, 30, 40, or newer) with Tensor Core support
- **NVIDIA Driver**: Game Ready Driver 530.xx or newer (NGX Core compatible)
- **Target Games**: Windowed or Borderless Fullscreen mode

---

## 📁 Repository Structure

```
VLSS5/
├── src/                          # Core C++ source files
│   ├── Main.cpp                  # Application entry point & modern UI
│   ├── App.cpp                   # Overlay window & render loop orchestration
│   ├── DLSSManager.cpp           # NVIDIA NGX DLSS Super Resolution engine
│   ├── DLSSNRManager.cpp         # NVIDIA NGX Neural Reconstruction pipeline
│   ├── MotionVectorManager.cpp   # Optical flow CS & reactive mask generator
│   ├── D3D12Interop.cpp          # High-performance D3D11-to-D3D12 resource bridge
│   ├── CaptureManager.cpp        # Windows Graphics Capture API wrapper
│   ├── Renderer.cpp              # Direct3D 11 presentation pipeline
│   ├── SettingsWindow.cpp        # Real-time Neural Configuration panel
│   ├── ConfigManager.cpp         # INI configuration manager
│   ├── InputForwarder.cpp        # Global hotkey hook & input management
│   └── WindowEnumerator.cpp      # Active window enumeration
├── dlssnr/                       # DLSS-NR Forwarder library source
│   └── forwarder/
│       └── dlssnr_forwarder.cpp  # NGX caller redirector module
├── app.ico                       # Multi-resolution application icon
├── logo_neon.png                 # Electric neon green branding asset
├── VLSS5.rc                      # Windows resource definition
├── VLSS5.sln                     # Visual Studio Solution
├── VLSS5.vcxproj                 # Visual Studio C++ Project
├── .gitignore                    # Git ignore specifications
└── LICENSE                       # MIT License
```

---

## 🛠️ Building from Source

### Prerequisites
- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community, Professional, or Enterprise)
- **Desktop development with C++** workload installed
- **Windows 10 / 11 SDK** (10.0.22000.0 or higher)

### Build Steps

1. **Clone the repository:**
   ```bash
   git clone https://github.com/vuenxx/VLSS5.git
   cd VLSS5
   ```

2. **Open the Solution:**
   - Double-click `VLSS5.sln` to open it in Visual Studio 2022.

3. **Build the Project:**
   - Set the configuration to **Release** and platform to **x64**.
   - Press **Ctrl + Shift + B** (or *Build > Build Solution*).
   - The compiled binary will be placed in `x64/Release/VLSS5.exe`.

---

## 🚀 Runtime Setup & Usage

To run `VLSS5.exe`, ensure the required NVIDIA NGX runtime libraries are present in the same directory as the executable:

1. **`nvngx_dlss.dll`**: Official NVIDIA DLSS Super Resolution binary (extractable from any DLSS-supported game or driver).
2. **`nvngx_dlssnr.dll`**: Official NVIDIA DLSS Ray Reconstruction / Neural Radiance binary.
3. **`nvngx.dll_dlssnr.dll`**: The forwarder library compiled from `dlssnr/forwarder/`.
4. **`nvofapi64.dll`**: NVIDIA Optical Flow API library.

### Starting VLSS5
1. Launch **`VLSS5.exe`** (Run as Administrator recommended for elevated GPU scheduling).
2. Select your running game from the **Target Application** list.
3. Press **BAŞLAT ►** or press the global capture toggle key (**Alt + S** by default).
4. Enjoy smooth, stabilized, AI-enhanced gameplay!

---

## 📜 License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

---

## ⚠️ Disclaimer

*NVIDIA, GeForce, GeForce RTX, and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries. This project is an independent open-source tool and is not affiliated with, endorsed by, or sponsored by NVIDIA Corporation.*

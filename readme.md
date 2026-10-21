# 🧩 Cross-Platform C++ Application Boilerplate

This project is a **cross-platform C++ desktop application** powered by:
- 🏗️ **CMake** — build configuration and management  
- 📦 **vcpkg** — package management and dependency resolution  
- 🖼️ **Slint** — declarative UI framework for native apps  
- 💻 **Clang / GCC / MSVC** — platform-specific toolchains  

Designed for **macOS, Linux, and Windows**, this setup supports both **Debug** and **Release** builds with clear structure and reproducible builds.

---

## 🗂️ Folder Structure

```
project-root/
│
├── CMakeLists.txt           # Main CMake entry point
├── vcpkg-configuration.json # Locks vcpkg version/baseline for reproducible builds
├── vcpkg.json               # vcpkg package manifest
│
├── cmake/                   # Custom CMake modules and toolchains
│   ├── toolchains/
│   │   ├── toolchain-macos.cmake
│   │   ├── toolchain-linux.cmake
│   │   └── toolchain-windows.cmake
│   └── modules/
│       └── FindSomething.cmake  # Example of a custom find module
│
├── include/                 # Public headers (available to other modules)
│   └── mylib/
│       └── my_header.hpp
│
├── src/                     # Application source code
│   ├── main.cpp
│   ├── App.cpp
│   └── App.hpp
│
├── assets/                  # UI files, icons, resources, etc.
│   ├── main.slint
│   └── icons/
│
├── .build/                   # Build artifacts (ignored in git)
│   ├── macos/
│   │   ├── Debug/
│   │   └── Release/
│   ├── linux/
│   └── windows/
│
└── docs/                    # Optional documentation and references
```

---

## ⚙️ Toolchain Configuration

Toolchain files are used to control the compiler, system root, and CMake settings for each platform.

### 🧱 macOS — `cmake/toolchains/toolchain-macos.cmake`

### 🐧 Linux — `cmake/toolchains/toolchain-linux.cmake`

### 🪟 Windows — `cmake/toolchains/toolchain-windows.cmake`

---

## 📦 Dependency Management (vcpkg)

This project uses **vcpkg** with a locked baseline for reproducible builds.

### `vcpkg-configuration.json`

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "92d1d0e657ab9ae9278dc5df76ebf6e1ebdf82a3"
  },
  "registries": []
}
```

## Getting Started 

### 1️⃣ Clone the repo

### 2️⃣ Bootstrap vcpkg

```bash
$ ./scripts/setup_vcpkg.sh
```

## 3️⃣ Configure the project
```bash
cmake --preset <preset_name>
```

Pick 1 from **CMake presets**

## 4️⃣ Build

```bash
cmake --build --preset <build_preset_name>
```

---

## 🧩 Folder Responsibilities Summary

| Folder | Purpose |
|:--------|:---------|
| `src/` | Core source code (.cpp, .hpp) |
| `include/` | Public headers for reusable libraries |
| `assets/` | UI files, images, etc. |
| `cmake/` | Custom toolchains and CMake modules |
| `build/` | Build output separated by platform/config |
| `docs/` | Documentation, guides, diagrams |

---

## 🚀 Updating vcpkg

To update dependencies or the vcpkg baseline:

### 1️⃣ Update the baseline commit in **vcpkg-configuration.json**
```json
{
  "default-registry": {
    "kind": "builtin",
    "baseline": "<add commit here>"
  },
  "registries": []
}

```

### 2️⃣ Re-run vcpkg bootstrap script
```bash
$ ./scripts/setup_vcpkg.sh
```

Then commit.

---

## 🧭 Future Improvements

- Add CI/CD using GitHub Actions or GitLab CI  
- Integrate code formatters (clang-format)  
- Add unit testing with Catch2 or GoogleTest  
- Package app installers (macOS `.app`, Windows `.exe`, etc.)

---

**Made with ❤️ using CMake, Slint, and modern C++.**

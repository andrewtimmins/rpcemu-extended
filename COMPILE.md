# How to Compile RPCEmu

RPCEmu (Spork Edition) is built using the Qt 5 framework. It supports compiling natively on Linux and Windows, as well as cross-compiling for Windows from Linux.

## Linux (Native)

### 1. Install Dependencies (Debian/Ubuntu)

You will need a C++ compiler, Qt 5 development libraries, and development headers for VNC support.

```bash
sudo apt-get update
sudo apt-get install build-essential qtbase5-dev qtbase5-dev-tools libvncserver-dev
```

*Note: On older Debian/Ubuntu versions, `qt5-default` might differ.*

### 2. Build

RPCEmu uses `qmake` to generate a Makefile.

```bash
# Navigate to the source directory
cd src/qt5

# Generate makefile
qmake rpcemu.pro

# Compile
make
```

The resulting executable `rpcemu-recompiler` (or `rpcemu-interpreter`) will be placed in the project root directory.

### 3. Run

```bash
cd ../..
./rpcemu-recompiler
```

---

## Windows (Native)

We recommend using **MSYS2** to provide a Unix-like build environment on Windows.

### 1. Install MSYS2

1.  Download and install MSYS2 from [msys2.org](https://www.msys2.org/).
2.  Run the **MSYS2 MSYS** terminal and update the package database:
    ```bash
    pacman -Syu
    ```
    *(You may need to restart the terminal and run it again)*

### 2. Install Dependencies available in the Mingw64 shell

Open the **MSYS2 MinGW 64-bit** terminal (this is important, do not use the standard MSYS terminal for this step) and install the necessary tools and Qt 5:

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-qt5
```

### 3. Build

In the same **MSYS2 MinGW 64-bit** terminal:

```bash
# Navigate to the RPCEmu source directory (adjust path as needed)
cd /c/Users/YourName/path/to/RPCEmu/src/qt5

# Generate Makefile
qmake rpcemu.pro

# Compile
mingw32-make
```

### 4. Run

You can run the executable directly from the terminal or usually from Windows Explorer, provided all necessary DLLs are in the path.

---

## Windows (Cross-Compilation from Linux)

This method allows you to build Windows executables (`.exe`) entirely from a Linux environment using **MXE (M Cross Environment)**.

### 1. Prerequisites (Debian/Ubuntu)

Use standard apt packages to install the build tools required to compile MXE itself.

```bash
sudo apt-get update
sudo apt-get install autoconf automake autopoint bash bison build-essential \
    bzip2 cmake flex g++ g++-multilib gettext git gperf intltool \
    libc6-dev-i386 libffi-dev libgdk-pixbuf2.0-dev libltdl-dev libssl-dev \
    libtool-bin libxml-parser-perl lzip make openssl p7zip-full patch perl \
    pkg-config python3 ruby scons sed unzip wget xz-utils
```

### 2. Install MXE

MXE builds standard Windows binaries (MinGW).

1.  **Clone MXE to `/opt`:**
    ```bash
    sudo git clone https://github.com/mxe/mxe.git /opt/mxe
    # Optional: Change ownership to yourself
    sudo chown -R $USER:$USER /opt/mxe
    ```

2.  **Build Qt5 (This takes a long time - hours):**
    This builds the cross-compiler and Qt library for Windows (32-bit).
    ```bash
    cd /opt/mxe
    make MXE_TARGETS='i686-w64-mingw32.static' qtbase qtmultimedia
    ```

3.  **Add to PATH:**
    ```bash
    export PATH=/opt/mxe/usr/bin:$PATH
    ```

### 3. Build RPCEmu

Once MXE is ready, build RPCEmu using the MXE-specific wrapper scripts:

```bash
# Navigate to src/qt5
cd src/qt5

# Run MXE qmake (32-bit Windows target)
/opt/mxe/usr/bin/i686-w64-mingw32.static-qmake-qt5 rpcemu.pro

# Compile
make
```

### 4. Verify

The resulting files will be `release/rpcemu-recompiler.exe` (or similar). You can copy these to a Windows machine to run them.

---

## macOS

*Build instructions for macOS are currently experimental. Standard Qt 5 build procedures (`qmake`, `make`) should work provided dependencies are installed via Homebrew.*

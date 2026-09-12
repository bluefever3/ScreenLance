# ScreenLance – Offline Real-Time Screen Translation

## Project Version
- **Version**: 1.0.0
- **Release Date**: July 2026
- **Target Platform**: Windows 11 (10.0.22000+)

---

## Dependencies & Versions

### System Requirements
- **Windows 11** (build 22000 or later)
  - UTF-8 console output (Windows 10.0.14393+) – for Turkish character support
- **Visual Studio 2022** (v17.0+) with C++ support
  - Platform Toolset: v145 (MSVC 14.3x)
  - Language Standard: `/std:c++latest`

### External Dependencies

#### vcpkg Packages (Manifest Mode)
Specified in `vcpkg.json` – **latest compatible versions automatically pulled**:

| Package | Purpose |
|---------|---------|
| `onnxruntime` | Neural machine translation inference |
| `sentencepiece` | Subword tokenization |
| `nlohmann-json` | Configuration JSON parsing |

**Install command:**
```powershell
cd C:\vcpkg
.\vcpkg install onnxruntime:x64-windows sentencepiece:x64-windows nlohmann-json:x64-windows
```

**To update to latest versions:**
```powershell
.\vcpkg update
.\vcpkg upgrade --no-dry-run
```

#### NuGet Packages
Specified in `ScreenLance.vcxproj` – **latest compatible version auto-pulled by Visual Studio**:

| Package | Purpose |
|---------|---------|
| `Microsoft.Windows.CppWinRT` | Windows Runtime C++ bindings |

**To pin specific versions** (optional):
- Edit `directory.packages.props`
- Add `<PackageVersion>` entries
- Rebuild solution

#### Windows SDK
- **Minimum**: Windows 10 SDK (10.0.22000)
- **Recommended**: Latest (installed with Visual Studio)

---

## Build Instructions

### Setup (One-time)
1. Install vcpkg dependencies:
   ```powershell
   cd C:\vcpkg
   .\vcpkg install onnxruntime:x64-windows sentencepiece:x64-windows nlohmann-json:x64-windows
   ```
2. Open `ScreenLance.sln` in Visual Studio 2022

### Build
```
Configuration: Release|x64
Build → Rebuild Solution
```

**Output**: `x64\Release\ScreenLance.exe` + `x64\Release\Models\*`

---

## Turkish Character Encoding – Fixed ✓

### Problem Resolved
Previous logs showed garbled output: "bÃ¶lge" (böyle), "Ã§eviri" (çeviri)

### Solution Applied
1. Logger calls `SetConsoleOutputCP(CP_UTF8)` at startup
2. UTF-8 → Wide string conversion for Visual Studio debugger
3. Log files saved as UTF-8 automatically

### Result
- Console output: "böyle çeviri gönderiliyor" ✓
- Log files: Turkish characters fully readable ✓

---

## Version Management

### Dynamic Updates
- **vcpkg packages**: `vcpkg.json` lists dependencies; vcpkg automatically fetches latest
- **NuGet packages**: Visual Studio automatically resolves latest compatible versions
- **Project metadata**: `directory.packages.props` available for optional version pinning

### To Pin Specific Versions (Optional)
Edit `directory.packages.props`:
```xml
<ItemGroup>
  <PackageVersion Include="Microsoft.Windows.CppWinRT" Version="2.0.240410.0" />
</ItemGroup>
```
Then rebuild solution.

---

## Features

### Implemented ✓
- Continuous translation (Ctrl+F8)
- One-shot translation (Ctrl+F9) – drag-to-select
- Real-time Turkish overlay
- Tray icon control
- UTF-8 diagnostic logging
- Precompiled headers (faster builds)

### Planned (Stubs)
- Settings UI (Ctrl+F10)
- Hotkey customization
- Target language selection

---

## Compatibility
- ✅ Windows 11 (22000+)
- ⚠️ Windows 10 (21H2+) – untested
- ❌ Older Windows 10 – no UTF-8 console support

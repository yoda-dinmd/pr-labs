# C++ Project Template (CMake + clangd)

This template provides a minimal and reproducible C++ development setup using CMake as the build system and clangd for editor integration and diagnostics.

It is intended to be copied for each new project with minimal modification.

---

# Project Structure

A typical project layout:

```
Template/
├── CMakeLists.txt
├── src/
│   └── main.cpp
├── include/
│   └── (header files)
└── build/
```

The `build/` directory is generated automatically and should not be edited manually.

---

# Creating a New Project

To start a new project, copy the template directory:

```bash
cp -r Template MyNewProject
cd MyNewProject
```

After copying, update the project name inside `CMakeLists.txt`.

Example:

```cmake
project(MyNewProject LANGUAGES CXX)

add_executable(MyNewProject
    src/main.cpp
)
```

The executable name must match all references in the CMake configuration.

---

# First-Time Configuration

Generate build files and compilation database for clangd:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

This step:
- Configures the project
- Generates build system files in `build/`
- Creates `compile_commands.json` for clangd

---

# Building the Project

Compile the project:

```bash
cmake --build build
```

This produces the executable defined in `CMakeLists.txt`.

---

# Running the Program

Run directly:

```bash
./build/MyNewProject
```

Or, if a run target exists:

```bash
cmake --build build --target run
```

---

# clangd Integration

clangd uses `compile_commands.json` for accurate code analysis.

Ensure the file exists:

```
build/compile_commands.json
```

VS Code configuration:

```json
{
  "clangd.arguments": [
    "--compile-commands-dir=build"
  ]
}
```

If diagnostics are incorrect, re-run the CMake configuration step.

---

# Reconfiguring the Project

If `CMakeLists.txt` changes:

```bash
cmake -S . -B build
```

If only source files change:

```bash
cmake --build build
```

---

# Cleaning the Build

To reset the build completely:

```bash
rm -rf build
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

---

# CMake Target Rules

The name used in `add_executable(...)` defines the build target.

All related CMake commands must use the same name consistently:

```cmake
add_executable(MyNewProject src/main.cpp)

target_include_directories(MyNewProject PRIVATE include)

target_compile_options(MyNewProject PRIVATE
    -Wall -Wextra -Wpedantic
)
```

---

# Typical Workflow

After setup, daily usage:

```bash
cmake --build build
./build/MyNewProject
```

Or, if a run target is defined:

```bash
cmake --build build --target run
```

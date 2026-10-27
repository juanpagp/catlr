# catlr

**catlr** is a powerful, POSIX command-line utility designed to list directory structures and print file contents recursively, specifically tailored for code auditing and project reviews. It intelligently falls back to native C++ implementations if preferred external tools (bat, tree, etc.) are unavailable, ensuring functionality on any system (Linux, macOS, Windows).

## Features

-   **Automatic Source Filtering (`.gitignore`):** For any specified directory, **catlr** automatically reads the local `.gitignore` file and applies its exclusion patterns to both the directory listing and the file content printing, allowing for cleaner and more relevant output by default.
    
-   **Intelligent Tool Fallback:** Prioritizes system commands (bat, tree) if installed, but seamlessly switches to built-in C++ methods for file reading and tree generation if external tools are missing or configured otherwise.
    
-   **Granular Filtering:** Offers fine-grained control over which files and directories are **listed** (in the tree) and which are **printed** (content display), using powerful include/exclude patterns with **wildcard support** and **case-sensitive direct matching**.
    
-   **Syntactic Sugar for Extensions:** Automatically converts single-dot extensions like `.cpp`, `.o`, or `.a` provided as arguments into wildcard patterns (`*.cpp`, `*.o`, `*.a`), simplifying command-line input.
    
-   **Cross-Platform Configuration:** Use a simple configuration file (`~/.config/catlr/catlr.conf`) to specify preferred external tools.
    
-   **I/O Loop Protection:** Automatically detects and skips printing any file that is also the destination of the program's output (`catlr ... > file.txt`), preventing infinite recursion errors.
    

## 🚀 Installation (From Source)

catlr is written in C++ and requires a C++17 compliant compiler (like g++ or clang++) due to its use of the $std::filesystem$ library.

Save the source code as `catlr.cpp` and compile using one of the following commands:

| Platform | Command | Notes |
| --- | --- | --- |
| **Linux (g++)** | `g++ -o catlr catlr.cpp -std=c++17 -lstdc++fs` | Uses the GNU standard filesystem library. |
| **macOS (clang++)** | `clang++ -o catlr catlr.cpp -std=c++17 -lc++fs` | Uses the LLVM standard filesystem library. |
| **Windows (MSVC)** | `cl.exe /std:c++17 /EHsc catlr.cpp` | Compiles using the Visual Studio compiler. |

After compilation, place the resulting executable (`catlr` or `catlr.exe`) in a directory listed in your system's `$PATH`.

## Configuration

You can change the default tools this command uses (`bat` or `cat` and `tree`) for the output, but you are free to override them. catlr looks for a configuration file to determine which external tools to use for directory listing and file printing.

**Configuration File Path:** `~/.config/catlr/catlr.conf`

### Setup Steps:

1.  `mkdir -p ~/.config/catlr`
    
2.  `nano ~/.config/catlr/catlr.conf`
    
3.      # catlr Configuration File
        # If a command is not found or this file is empty, catlr uses its native C++ implementation.
        # Use 'lsd --tree' for directory listing (or 'tree')
        treePrintCommand=lsd --tree
        # Use 'bat' for file printing (or 'cat')
        filePrintCommand=bat
        
    

## 📖 Usage and Filtering

The general syntax is: `catlr [directory_path...] [filter_rules...]`

This structure allows you to specify **one or more starting directories** followed by any combination of filtering flags and their associated patterns. The filters are applied globally across all specified directories.

### Automatic Source Filtering

By default, **catlr** now looks for a `.gitignore` file in the root of every target directory and automatically adds its exclusions to the filter list.

| Flag | Description |
| --- | --- |
| **`--no-gitignore`** | Disable automatic `.gitignore` parsing for the current execution. |

### Core Filtering Flags

Filtering is divided into **List** (tree output) and **Print** (file content output). The fundamental logic is that **Include always overrides Exclude**.

| Base Flag | Modifier (Scope) | Permutation (Description) |
| --- | --- | --- |
| **`-e`** (Exclude) | **(None)** | Exclude the **following patterns** from **both** listing and printing. |
|  | **`-l`** (List) | Exclude the **following patterns** from **listing only** (`-el` or `-le`). |
|  | **`-p`** (Print) | Exclude the **following patterns** from **printing only** (`-ep` or `-pe`). |
| **`-i`** (Include) | **(None)** | Include the **following patterns** in **both** listing and printing (overrides any Exclude). |
|  | **`-l`** (List) | Include the **following patterns** in **listing only** (`-il` or `-li`). |
|  | **`-p`** (Print) | Include the **following patterns** in **printing only** (`-ip` or `-pi`). |

> **Note:** The order of the modifier letters (`-el` vs `-le`, `-ip` vs `-pi`) does not matter. Multiple patterns can be listed after a single flag, or the flag can be repeated for each pattern.

### Examples

#### 1\. Zero-Config Audit (using `.gitignore`)

If your target directory has a `.gitignore` file, you can now audit its source code without any explicit exclusion flags.

    # catlr automatically respects build/, node_modules/, *.o, etc.
    catlr dev/my-project/
    

#### 2\. Multi-Directory Scan and Filtering

Audit code across two separate repositories (`/src/backend` and `/src/frontend`), excluding common build artifacts but ensuring all Java and C++ files are printed.

    # Note: '.java' and '.cpp' are automatically treated as '*.java' and '*.cpp'
    catlr /src/backend /src/frontend -e node_modules/ build/ -ip .java .cpp
    

#### 3\. Global Exclusion (Ignoring Development Artifacts)

Completely ignore the `node_modules` directory and any files containing `build.log` in their name (using wildcards) in both the directory tree and the printed content.

    catlr . -e node_modules/ *build.log*
    

#### 4\. Selective Content Audit (Print Includes)

List the entire project structure, but only print the contents of C++ source files (`.cpp` and `.h`).

    # Automatically treated as '*.cpp' and '*.h'
    catlr . -ip .cpp .h
    

#### 5\. Exception to Exclusion (The "Exclude all but one" pattern)

Exclude the entire `build/` directory from being processed, but explicitly **include** and print the critical file located at `build/main.js`.

    catlr . -e build/ -i build/main.js
    

## ⚠️ Important Note on Filtering

The filtering is performed via **case-sensitive matching** against the full path string.

-   **Direct Path Matching:** A pattern without wildcards (`*`) must match the path segment exactly. A trailing slash (`/`) is highly recommended to target directories recursively.
    
    -   `-e modules` will **only** exclude the file named `modules` in the current directory.
        
    -   `-e modules/` will exclude (recursively) the directory `modules/` and all its contents.
        
    -   `-e src/models/user.js` targets that specific file path.
        
-   **Wildcard Matching:** Use the asterisk (`*`) for general substring or suffix/prefix matching.
    
    -   `-e *modules*` will exclude every file or directory that contains `modules` in its name (e.g., `node_modules/`, `my_modules.txt`, `modules.cpp`).
        
    -   `-ip *.log` targets all files **ending with** `.log` for printing.
Basic C-Compiler Student Project. Goes through Various Compilation Phases such as
Lexer --> Parser --> Semantic Analysis --> Code Generation via LLVM

Windows:
Get mingw-64 shell for window. Install llvm, and gcc in mingw-64 shell.

For linux, install llvm and gcc:
Ubuntu:
sudo apt install build-essential llvm llvm-dev clang

Fedora:
sudo dnf install @development-tools gcc gcc-c++ llvm llvm-devel clang

Arch:
sudo pacman -Syu base-devel llvm clang

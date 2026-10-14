# Dx12 Template Code

Empty Dx12 project with path tracer.

## Build Instructions

For the first build, it's recommended to **run the Visual Studio as an Administrator**. Build process will create symbolic links to folder with shaders, which can fail on some systems if VS is run without administrator rights.

* Run `cmake` on the *src* subfolder and build the generated project
* Or run provided file `win_cmake_create_vs_project.bat` and build the project generated in the *vsbuild* subfolder

### Troubleshooting

If you get an error *Error: failed to get shader blob result!*, it most likely means that build didn't create a symbolic link for shaders folder in the build directory and executable can't find shader files. To fix this, run Visual Studio as administrator, clean the solution and run the build again.

## 3rd Party Software

This project uses following dependencies:

* [d3dx12.h](https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3DX12), provided with an MIT license. 
* [STB library](https://github.com/nothings/stb/), provided with an MIT license.
* [DXC Compiler](https://github.com/microsoft/DirectXShaderCompiler), provided with an University of Illinois Open Source
* [ImGUI](https://github.com/ocornut/imgui), provided with an MIT license.
* [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader), provided with an MIT license.
* [TinyGLTF](https://github.com/syoyo/tinygltf), provided with an MIT license.
* [GLM library](https://github.com/g-truc/glm), provided with an MIT license.

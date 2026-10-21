# Dx12 Template Code

Empty Dx12 project with path tracer.

## Build Instructions

* Run `cmake` on the *src* subfolder and build the generated project
* Or run provided file `win_cmake_create_vs_project.bat` and build the project generated in the *vsbuild* subfolder

The application will look for assets and shaders in their respective folders next to executable. If not found, it will look inside development sources under *src* folder. This makes sure that for development you only need ony copy of shaders, inside the src folder.

### Troubleshooting

If you get an error *Error: failed to get shader blob result!*, the executable could not find shader files. Check whether shaders folder is present next to executable, or inside development sources.

## Scripts

| Script | Purpose |
| --- | --- |
| `win_cmake_create_vs_project.bat` | Generate the Visual Studio 2022 x64 project in `vsbuild`. |
| `win_cmake_build.bat` | Build Release in an already configured `vsbuild` directory. |
| `win_run.bat` | Configure, build Release, and run the executable from `vsbuild`. |
| `win_cmake_deploy.bat` | Configure, build Release, and install the executable, DXC DLLs, shaders, Cornell box, and license into `install`. |
| `package_source.bat` | Create a source ZIP, excluding SDK dependencies, assets, and build output.

## 3rd Party Software

This project uses following dependencies:

* [d3dx12.h](https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3DX12), provided with an MIT license. 
* [STB library](https://github.com/nothings/stb/), provided with an MIT license.
* [DXC Compiler](https://github.com/microsoft/DirectXShaderCompiler), provided with an University of Illinois Open Source
* [ImGUI](https://github.com/ocornut/imgui), provided with an MIT license.
* [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader), provided with an MIT license.
* [TinyGLTF](https://github.com/syoyo/tinygltf), provided with an MIT license.
* [GLM library](https://github.com/g-truc/glm), provided with an MIT license.

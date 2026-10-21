cmake_minimum_required (VERSION 3.21)
project (Dx12Template LANGUAGES CXX)

# Include macros
include(${CMAKE_SOURCE_DIR}/../cmake/macros.cmake)
 
# Additional places where to look for Find*.cmake scripts
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/../cmake/")
  
# Get subdirectories in sdk folder
SUBDIRLIST(SDK_SUBDIRS "${CMAKE_SOURCE_DIR}/../sdk")

# Create a list of sdk subdirs (including sdk folder)
SET(SDK_SUBDIRS_CONCAT "${CMAKE_SOURCE_DIR}/../sdk")
FOREACH(subdir ${SDK_SUBDIRS})
    set(SDK_SUBDIRS_CONCAT ${SDK_SUBDIRS_CONCAT} "${CMAKE_SOURCE_DIR}/../sdk/${subdir}")
ENDFOREACH()
    
print_list_contents("SDK Subdirectories:" SDK_SUBDIRS_CONCAT)

# Additional places where to look for libraries - all our SDK folders
set(CMAKE_PREFIX_PATH ${SDK_SUBDIRS_CONCAT} CACHE PATH "Paths to dependencies")
               
set(INCS
    LightsExtractor.h
    common.h
    scene.h
    TinyGLTF.h
    shaders/brdf.h
    shaders/shared.h
    thirdparty/tinygltf/tiny_gltf.h
    utils.h
    D3D12Helpers.h
    GPUProfiler.h
    TAA.h
    RTPass.h
    ComputePass.h
    DxcShaderCompiler.h
    D3D12ResourceUploader.h)

set(CPPS
    LightsExtractor.cpp
    DxcShaderCompiler.cpp
    D3D12Helpers.cpp
    D3D12ResourceUploader.cpp
    GPUProfiler.cpp
    TAA.cpp
    RTPass.cpp
    ComputePass.cpp
    main.cpp
    scene.cpp
    STB.cpp
    TinyGLTF.cpp
    TinyObjLoader.cpp
    utils.cpp)

set(SHADERS
    shaders/AO.hlsl
    shaders/AutoExposure.hlsl
    shaders/Color.hlsl
    shaders/Debug.hlsl
    shaders/GBuffer.hlsl
    shaders/GBufferPass.hlsl
    shaders/Lights.hlsl
    shaders/PathTracer.hlsl
    shaders/RaytracedAO.hlsl
    shaders/RaytracedDI.hlsl
    shaders/Resources.hlsl
    shaders/RNG.hlsl
    shaders/RTHelpers.hlsl
    shaders/RTUtils.hlsl
    shaders/TAA.hlsl
    shaders/Tonemapping.hlsl)

# Create folders in VS Project
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INCS} ${CPPS} ${SHADERS})
       
find_package(GLM REQUIRED)    
find_package(STB REQUIRED)                             
find_package(ImGui REQUIRED Sources)
find_package(D3DX12 REQUIRED)
find_package(DXC REQUIRED)
find_package(TINYOBJ REQUIRED)

include_directories(${GLM_INCLUDE_DIRS}) 
include_directories(${STB_INCLUDE_DIRS}) 
include_directories(${TINY_OBJ_LODAER_INCLUDE_DIR}) 
include_directories(${ImGui_INCLUDE_DIR}) 
include_directories(${D3DX12_INCLUDE_DIR}) 
include_directories(${DXC_INCLUDE_DIR}) 

# Put imgui files into separate VS folder
FILE(GLOB_RECURSE IMGUI_SRCS 
      ${ImGui_INCLUDE_DIR}/*.cpp 
      ${ImGui_INCLUDE_DIR}/*.h) 

SOURCE_GROUP(imgui FILES ${IMGUI_SRCS})

# Set Unicode support for MSVC project
if(MSVC)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

add_definitions(-DXC_DONT_COMPILE_FXC)

set(CMAKE_CONFIGURATION_TYPES Debug Release)

add_executable(Dx12Template WIN32 ${CPPS} ${INCS})

# Compile source files in parallel.
if(MSVC)
    target_compile_options(Dx12Template PRIVATE /MP)
endif()

# Precompile headers shared by the renderer source files.
target_precompile_headers(Dx12Template PRIVATE common.h)

# ImGui compiles directly into the executable without the renderer's PCH.
set_source_files_properties(${ImGui_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Make this C++17 project
target_compile_features(Dx12Template PRIVATE cxx_std_17)

# Hardcoded assets and shaders folders used for development in case proper one is not present
target_compile_definitions(Dx12Template PRIVATE ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
target_compile_definitions(Dx12Template PRIVATE SHADERS_DIR="${PROJECT_SOURCE_DIR}/shaders")

# Set default VS project 
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Dx12Template)

# Add custom target to include shaders in Visual Studio project (SOURCE_GROUP only works when source files are actually used in some target)
add_custom_target(shaders SOURCES ${SHADERS})

target_link_libraries(Dx12Template PRIVATE ImGui::Sources ${DXC_LIBRARY})

# Copy dlls to target dir
add_custom_command(TARGET Dx12Template POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${DXC_DLL_DIR}/dxcompiler.dll"    
        $<TARGET_FILE_DIR:Dx12Template>)

add_custom_command(TARGET Dx12Template POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${DXC_DLL_DIR}/dxil.dll"    
        $<TARGET_FILE_DIR:Dx12Template>)

# Install the executable and the content (shaders + cornell box)
install(TARGETS Dx12Template RUNTIME DESTINATION .)
install(FILES "${DXC_DLL_DIR}/dxcompiler.dll" "${DXC_DLL_DIR}/dxil.dll" DESTINATION .)
install(DIRECTORY shaders DESTINATION . FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.h")
install(DIRECTORY assets/cornell_box DESTINATION assets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" DESTINATION .)
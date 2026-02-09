cmake_minimum_required(VERSION 3.17)

# 프로젝트 이름 설정
project(VulkanHybridPipeline LANGUAGES CXX)

# C++ 표준 설정 (C++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================================
# [추가됨] MSVC 인코딩 경고(C4819) 해결 옵션
# 소스 코드를 UTF-8로 해석하도록 강제합니다.
# =========================================================
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ----------------------------------------------------------------------------
# 1. Vulkan 패키지 찾기 (SDK)
# ----------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# ----------------------------------------------------------------------------
# 2. 쉐이더 컴파일 설정 (GLSLC 사용)
# ----------------------------------------------------------------------------
find_program(GLSLC_EXECUTABLE glslc HINTS ${Vulkan_GLSLC_EXECUTABLE})

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found! Please install Vulkan SDK.")
endif()

# shaders 폴더 안의 레이 트레이싱 쉐이더 찾기
file(GLOB RAY_TRACING_SHADERS 
    "shaders/*.rgen" 
    "shaders/*.rmiss" 
    "shaders/*.rchit"
)

set(SPV_SHADERS "")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# 각 쉐이더를 SPV로 컴파일
foreach(SHADER_SOURCE ${RAY_TRACING_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
    set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
    
    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SPV_OUTPUT} --target-env=vulkan1.2
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling shader: ${SHADER_NAME} to ${SHADER_NAME}.spv"
    )
    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

# ----------------------------------------------------------------------------
# 3. 실행 파일 및 소스 코드
# ----------------------------------------------------------------------------
set(SOURCE_FILES 
    Vulkan_ex01.cpp 
    tiny_obj_loader.h
)

# 실행 파일 생성
add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${SPV_SHADERS})

# ----------------------------------------------------------------------------
# 4. [사용자 설정] 라이브러리 절대 경로 설정
# ----------------------------------------------------------------------------
set(LIB_ROOT "C:/Users/leeso/Documents/Visual Studio 2022/Libraries")

# 사용자가 지정한 폴더명
set(GLFW_PATH "${LIB_ROOT}/glfw-3.4.bin.WIN64")
set(GLM_PATH  "${LIB_ROOT}/glm-1.0.2")

# 경로 확인 로그 출력
message(STATUS "GLFW Path: ${GLFW_PATH}")
message(STATUS "GLM Path: ${GLM_PATH}")

# ----------------------------------------------------------------------------
# 5. 인클루드 및 라이브러리 연결
# ----------------------------------------------------------------------------
# 헤더 경로 추가
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}  # tiny_obj_loader.h 위치
    ${Vulkan_INCLUDE_DIRS}       # Vulkan 헤더
    "${GLFW_PATH}/include"       # GLFW 헤더
    "${GLM_PATH}"                # GLM 헤더
)

# 라이브러리 파일 연결
target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    "${GLFW_PATH}/lib-vc2022/glfw3.lib"
)

# ----------------------------------------------------------------------------
# 6. 리소스 복사 (Models 폴더)
# ----------------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/models"
    "${CMAKE_BINARY_DIR}/models"
    COMMENT "Copying models directory to build folder"
)
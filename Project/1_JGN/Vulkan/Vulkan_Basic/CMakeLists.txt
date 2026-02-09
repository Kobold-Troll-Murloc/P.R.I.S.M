cmake_minimum_required(VERSION 3.20)
project(Vulkan_Basic_Example)

# 1. C++ 표준 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. Vulkan 찾기 (자동 탐색)
find_package(Vulkan REQUIRED)

# 3. 실행 파일 정의
add_executable(VulkanApp ex01.cpp)

# =========================================================
# 4. [수정됨] 라이브러리 경로 설정
# 프로젝트 루트의 Dependencies 폴더를 참조합니다.
# =========================================================
set(LIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies")

# 폴더 이름이 중요합니다!
set(GLFW_PATH "${LIB_ROOT}/glfw-3.4.bin.WIN64")
set(GLM_PATH  "${LIB_ROOT}/glm")

# 경로가 잘 잡혔는지 확인하기 위한 로그 출력
message(STATUS "GLFW 경로: ${GLFW_PATH}")
message(STATUS "GLM 경로: ${GLM_PATH}")

# 5. 헤더 경로 추가
target_include_directories(VulkanApp PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    "${GLFW_PATH}/include"   # include 폴더 안의 GLFW/glfw3.h 를 찾음
    "${GLM_PATH}"            # glm-1.0.2 폴더 안의 glm/glm.hpp 를 찾음
)

# 6. 라이브러리 연결
target_link_libraries(VulkanApp PRIVATE
    Vulkan::Vulkan
    "${GLFW_PATH}/lib-vc2022/glfw3.lib"
)

# =========================================================
# 7. [추가] 쉐이더 자동 컴파일 설정 (Shader Compilation)
# =========================================================

# 1. glslc 컴파일러 찾기 (Vulkan SDK 안에 있음)
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS $ENV{VULKAN_SDK}/Bin)

# 2. 쉐이더 폴더 경로 설정
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# 3. 컴파일 함수 정의
function(compile_shader SHADER_SOURCE OUTPUT_SPV)
    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling shader: ${SHADER_SOURCE} -> ${OUTPUT_SPV}"
    )
endfunction()

# 4. 각 쉐이더 컴파일 명령 등록
# (코드에서 "shaders/vert.spv"로 읽으므로 그 경로에 맞게 생성)
set(VERT_SPV "${SHADER_DIR}/vert.spv")
set(FRAG_SPV "${SHADER_DIR}/frag.spv")

compile_shader("${SHADER_DIR}/shader.vert" ${VERT_SPV})
compile_shader("${SHADER_DIR}/shader.frag" ${FRAG_SPV})

# 5. 실행 파일이 쉐이더 컴파일을 기다리도록 설정
add_custom_target(Shaders DEPENDS ${VERT_SPV} ${FRAG_SPV})
add_dependencies(VulkanApp Shaders)



# =========================================================
# 8. [핵심 해결책] 쉐이더 폴더를 "실행 파일 옆"으로 복사하기
# =========================================================
add_custom_command(TARGET VulkanApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "$<TARGET_FILE_DIR:VulkanApp>/shaders"
    COMMENT "쉐이더 파일을 실행 경로로 복사합니다..."
)
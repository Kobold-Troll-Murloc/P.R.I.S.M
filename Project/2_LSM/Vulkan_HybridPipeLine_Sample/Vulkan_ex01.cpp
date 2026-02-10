#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
#include <glm/gtc/quaternion.hpp>


#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "VulkanProfiler.h"

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <array>
#include <optional>
#include <set>
#include <unordered_map>

const uint32_t WIDTH = 1280;
const uint32_t HEIGHT = 720;
const int MAX_FRAMES_IN_FLIGHT = 2;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct Camera {
    glm::vec3 position = glm::vec3(0.0f, 7.0f, 15.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float speed = 15.0f;
    float angularSpeed = 90.0f;
};

struct Light {
    glm::vec3 position;
    float intensity;
    glm::vec3 color;
    int enabled;
};

struct UniformBufferObject {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
    glm::vec3 cameraPos;
    float padding1;
    Light lights[3];
    int lightCount;
    float padding2[3];
};

struct Vertex {
    glm::vec3 pos;
    float pad1; // [추가] 12 + 4 = 16 bytes
    glm::vec3 normal;
    float pad2; // [추가] 12 + 4 = 16 bytes. 총 32 bytes (stride)

    bool operator==(const Vertex& other) const {
        return pos == other.pos && normal == other.normal;
    }

};

namespace std {
    template<> struct hash<Vertex> {
        size_t operator()(Vertex const& vertex) const {
            return (hash<glm::vec3>()(vertex.pos) ^
                (hash<glm::vec3>()(vertex.normal) << 1));
        }
    };
}

// [추가] 래스터화 쉐이더에 전달할 UBO (카메라 정보)
struct RasterUBO {
    glm::mat4 view;
    glm::mat4 proj;
};

// [추가] 물체마다 다르게 전달할 푸시 상수 (위치, 색상)
struct RasterPushConstant {
    glm::mat4 model;
    glm::vec3 color;
};


struct ObjectInstance {
    std::string modelPath;
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
    glm::vec3 color;
    bool isRaster = false; // true면 래스터화로, false면 레이트레이싱으로 (혹은 둘다)
};

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct AccelerationStructureBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;

};

struct GeometryData {
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexMemory;
    uint32_t vertexCount;
    uint32_t indexCount;
    VkAccelerationStructureKHR blas;
    VkBuffer blasBuffer;
    VkDeviceMemory blasMemory;
};

// 쉐이더(GLSL)와 데이터 레이아웃을 맞추기 위한 구조체
struct ObjState {
    glm::mat4 model;    // 64 bytes
    glm::vec4 position; // 16 bytes (xyz: pos, w: scale/padding)
    glm::vec4 velocity; // 16 bytes (xyz: vel, w: padding)
    glm::vec4 color;    // 16 bytes
}; // Total: 112 bytes (align 맞춰짐)

class RayTracedScene {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window;
    Camera camera;
    float deltaTime = 0.0f;
    float lastFrame = 0.0f;
    bool keys[1024] = { false };

    bool isLightOn = true;
    bool lKeyPressed = false; // 토글 입력을 위한 디바운싱 변수

    bool rightMouseButtonPressed = false;
    bool firstMouse = true;
    float lastX = WIDTH / 2.0f;
    float lastY = HEIGHT / 2.0f;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;

    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;
    bool framebufferResized = false;

    VkImage storageImage;
    VkDeviceMemory storageImageMemory;
    VkImageView storageImageView;
    VkDescriptorSetLayout rtDescriptorSetLayout;
    VkDescriptorPool rtDescriptorPool;
    std::vector<VkDescriptorSet> rtDescriptorSets;
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;

    VkPipeline rtPipeline;
    VkPipelineLayout rtPipelineLayout;
    VkBuffer raygenShaderBindingTable;
    VkDeviceMemory raygenShaderBindingTableMemory;
    VkBuffer missShaderBindingTable;
    VkDeviceMemory missShaderBindingTableMemory;
    VkBuffer hitShaderBindingTable;
    VkDeviceMemory hitShaderBindingTableMemory;

    VkStridedDeviceAddressRegionKHR raygenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callableRegion{};

    std::vector<ObjectInstance> objects;
    std::vector<VkAccelerationStructureKHR> bottomLevelAS;
    VkAccelerationStructureKHR topLevelAS;
    std::vector<Light> lights;

    std::vector<GeometryData> geometryDataList;
    AccelerationStructureBuffer tlasBuffer;
    VkBuffer instanceBuffer;
    VkDeviceMemory instanceMemory;

    struct ObjDesc {
        uint64_t vertexAddress;
        uint64_t indexAddress;
    };
    VkBuffer objDescBuffer;
    VkDeviceMemory objDescBufferMemory;

    VkBuffer instanceColorBuffer;
    VkDeviceMemory instanceColorMemory;

    // --- [추가] 래스터화(Rasterization) 관련 변수들 ---
    VkRenderPass renderPass;
    VkPipelineLayout graphicsPipelineLayout;
    VkPipeline graphicsPipeline;

    std::vector<VkFramebuffer> swapChainFramebuffers;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;
    // ------------------------------------------------

    // --- [추가] 래스터화 디스크립터 관련 ---
    VkDescriptorSetLayout rasterDescriptorSetLayout;
    VkDescriptorPool rasterDescriptorPool;
    std::vector<VkDescriptorSet> rasterDescriptorSets;

    std::vector<VkBuffer> rasterUniformBuffers;
    std::vector<VkDeviceMemory> rasterUniformBuffersMemory;
    std::vector<void*> rasterUniformBuffersMapped;
    // ------------------------------------

    // [추가] 클래스 멤버 변수
    VkSampler depthSampler;


    // -------- [프로파일링 관련] --------
    VulkanProfiler profiler;
    float titleUpdateTimer = 0.0f;
    // ------------------------------------


    // -------- [Compute 관련] --------

    // Compute 관련 변수
    VkPipeline computePipeline;
    VkPipelineLayout computePipelineLayout;
    VkDescriptorSetLayout computeDescriptorSetLayout;
    VkDescriptorPool computeDescriptorPool;
    VkDescriptorSet computeDescriptorSet;

    // 시뮬레이션용 SSBO (모든 오브젝트 정보 저장)
    VkBuffer objectSSBO;
    VkDeviceMemory objectSSBOMemory;

    // ------------------------------------


    void initWindow() {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(WIDTH, HEIGHT, "Ray Traced Scene", nullptr, nullptr);
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetKeyCallback(window, keyCallback);

        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPositionCallback);
    }

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<RayTracedScene*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
    }

    /*static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        auto app = reinterpret_cast<RayTracedScene*>(glfwGetWindowUserPointer(window));

        if (action == GLFW_PRESS) {
            app->keys[key] = true;

        }
        else if (action == GLFW_RELEASE) {
            app->keys[key] = false;
        }
    }*/

    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        auto app = reinterpret_cast<RayTracedScene*>(glfwGetWindowUserPointer(window));

        if (action == GLFW_PRESS) {
            app->keys[key] = true;

            if (key == GLFW_KEY_L) {
                app->isLightOn = !app->isLightOn;
                std::cout << "Light Toggled: " << (app->isLightOn ? "ON" : "OFF") << std::endl;
            }
        }
        else if (action == GLFW_RELEASE) {
            app->keys[key] = false;
        }
    }

    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
        auto app = reinterpret_cast<RayTracedScene*>(glfwGetWindowUserPointer(window));

        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) {
                app->rightMouseButtonPressed = true;
                app->firstMouse = true;

                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
            else if (action == GLFW_RELEASE) {
                app->rightMouseButtonPressed = false;

                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }

    static void cursorPositionCallback(GLFWwindow* window, double xpos, double ypos) {
        auto app = reinterpret_cast<RayTracedScene*>(glfwGetWindowUserPointer(window));

        if (!app->rightMouseButtonPressed) return;

        if (app->firstMouse) {
            app->lastX = xpos;
            app->lastY = ypos;
            app->firstMouse = false;
        }

        float xoffset = xpos - app->lastX;
        float yoffset = app->lastY - ypos;
        app->lastX = xpos;
        app->lastY = ypos;

        float sensitivity = 0.1f;
        xoffset *= sensitivity;
        yoffset *= sensitivity;

        app->camera.yaw += xoffset;
        app->camera.pitch += yoffset;

        if (app->camera.pitch > 89.0f) app->camera.pitch = 89.0f;
        if (app->camera.pitch < -89.0f) app->camera.pitch = -89.0f;

        glm::vec3 front;
        front.x = cos(glm::radians(app->camera.yaw)) * cos(glm::radians(app->camera.pitch));
        front.y = sin(glm::radians(app->camera.pitch));
        front.z = sin(glm::radians(app->camera.yaw)) * cos(glm::radians(app->camera.pitch));
        app->camera.front = glm::normalize(front);
    }


    GeometryData loadGeometry(const std::string& path, const glm::vec3& scale) {

        GeometryData geoData{};
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn, err;

        bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str());
        if (!ok) {
            throw std::runtime_error("failed to load obj: " + path + " warn=" + warn + " err=" + err);
        }

        std::unordered_map<Vertex, uint32_t> uniqueVertices{};
        bool hasNormals = false;

        for (const auto& shape : shapes) {
            for (const auto& index : shape.mesh.indices) {
                Vertex v{};
                v.pos = {
                    attrib.vertices[3 * index.vertex_index + 0] * scale.x,
                    attrib.vertices[3 * index.vertex_index + 1] * scale.y,
                    attrib.vertices[3 * index.vertex_index + 2] * scale.z
                };
                if (index.normal_index >= 0) {
                    v.normal = {
                        attrib.normals[3 * index.normal_index + 0],
                        attrib.normals[3 * index.normal_index + 1],
                        attrib.normals[3 * index.normal_index + 2]
                    };
                    hasNormals = true;
                }
                else {
                    v.normal = { 0.0f, 0.0f, 0.0f };
                }
                if (uniqueVertices.count(v) == 0) {
                    uniqueVertices[v] = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(v);
                }
                indices.push_back(uniqueVertices[v]);
            }
        }
        if (!hasNormals) {
            for (size_t i = 0; i < indices.size(); i += 3) {
                uint32_t idx0 = indices[i];
                uint32_t idx1 = indices[i + 1];
                uint32_t idx2 = indices[i + 2];

                glm::vec3 v0 = vertices[idx0].pos;
                glm::vec3 v1 = vertices[idx1].pos;
                glm::vec3 v2 = vertices[idx2].pos;

                glm::vec3 edge1 = v1 - v0;
                glm::vec3 edge2 = v2 - v0;

                glm::vec3 faceNormal = glm::cross(edge1, edge2);

                vertices[idx0].normal += faceNormal;
                vertices[idx1].normal += faceNormal;
                vertices[idx2].normal += faceNormal;
            }

            for (auto& v : vertices) {
                if (glm::length(v.normal) > 0.0f) {
                    v.normal = glm::normalize(v.normal);
                }
                else {
                    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }
            }
            std::cout << "Computed smooth normals for: " << path << std::endl;
        }

        geoData.vertexCount = vertices.size();
        geoData.indexCount = indices.size();

        VkDeviceSize vertexBufferSize = sizeof(Vertex) * vertices.size();
        createBuffer(vertexBufferSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            geoData.vertexBuffer, geoData.vertexMemory);

        void* data;
        vkMapMemory(device, geoData.vertexMemory, 0, vertexBufferSize, 0, &data);
        memcpy(data, vertices.data(), vertexBufferSize);
        vkUnmapMemory(device, geoData.vertexMemory);

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        createBuffer(indexBufferSize,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            geoData.indexBuffer, geoData.indexMemory);

        vkMapMemory(device, geoData.indexMemory, 0, indexBufferSize, 0, &data);
        memcpy(data, indices.data(), indexBufferSize);
        vkUnmapMemory(device, geoData.indexMemory);

        return geoData;
    }


    void initVulkan() {
        // =================================================================
        // 1. Vulkan 기본 인프라 구축 (Base Infrastructure)
        // =================================================================
        // Vulkan을 사용하기 위한 가장 기초적인 객체들을 생성합니다.
        createInstance();           // Vulkan 라이브러리 초기화 및 인스턴스 생성
        setupDebugMessenger();      // 검증 레이어(Validation Layer) 로그 설정
        createSurface();            // 윈도우(GLFW)와 Vulkan을 연결하는 표면 생성
        pickPhysicalDevice();       // 그래픽 카드(GPU) 선택
        createLogicalDevice();      // 논리적 장치 및 큐(Queue) 생성
        createSwapChain();          // 화면 출력을 위한 이미지 버퍼 체인 생성
        createImageViews();         // 스왑체인 이미지를 뷰(View) 형태로 래핑
        createCommandPool();        // 명령 버퍼(Command Buffer)를 할당할 풀 생성

        // =================================================================
        // 2. 씬 데이터 로드 (Scene Data Loading) - [매우 중요]
        // =================================================================
        // 물체의 개수, 초기 위치 등을 CPU 메모리(vector)에 적재합니다.
        // 이후 모든 버퍼(SSBO, AS 등)의 크기가 이 데이터에 의해 결정되므로 가장 먼저 해야 합니다.
        setupScene();

        // =================================================================
        // 3. 시뮬레이션 데이터 구축 (Compute & SSBO)
        // =================================================================
        // 물리 연산을 위한 데이터와 파이프라인을 만듭니다.

        // [순서 중요] setupScene() 데이터로 SSBO(GPU 버퍼)를 먼저 만듭니다.
        // 그래야 뒤에서 Raster/Compute 디스크립터 셋에 이 버퍼를 연결할 수 있습니다.
        createObjectSSBO();

        // Compute Shader 파이프라인 생성 (SSBO를 바인딩)
        createComputePipeline();

        // =================================================================
        // 4. 래스터화 리소스 준비 (Rasterization Resources)
        // =================================================================
        // 화면에 1차적으로 물체를 그리기 위한 준비 단계입니다.

        createDepthResources();     // 깊이 버퍼(Depth Buffer) 생성 (앞/뒤 구분용)
        createDepthSampler();       // 깊이 버퍼를 나중에 RT에서 읽을 때 사용할 샘플러
        createRenderPass();         // 그리기 작업의 흐름(첨부물 로드/저장 등) 정의
        createFramebuffers();       // RenderPass와 이미지를 연결하는 프레임버퍼 생성

        // =================================================================
        // 5. 래스터화 파이프라인 및 디스크립터 (Raster Pipeline & Descriptors)
        // =================================================================
        // 쉐이더가 사용할 데이터(UBO, SSBO) 연결 고리를 만듭니다.

        createRasterDescriptorSetLayout(); // 쉐이더 입력 구조 정의 (Binding 0: UBO, Binding 1: SSBO)
        createGraphicsPipeline();          // 버텍스/프래그먼트 쉐이더 컴파일 및 파이프라인 생성

        createRasterUniformBuffers();      // 카메라 정보(View/Proj) 담을 UBO 생성
        createRasterDescriptorPool();      // 디스크립터 셋을 찍어낼 풀 생성

        // [핵심 수정] 반드시 createObjectSSBO() 이후에 호출되어야 합니다!
        // 이제 SSBO가 존재하므로, 안전하게 디스크립터 셋(Binding 1)에 연결할 수 있습니다.
        createRasterDescriptorSets();

        // =================================================================
        // 6. 레이 트레이싱 리소스 준비 (Ray Tracing Resources)
        // =================================================================
        // 래스터화된 결과 위에 고품질 효과를 얹기 위한 준비입니다.

        createInstanceColorBuffer();       // RT에서 쓸 색상 정보 버퍼
        createStorageImage();              // RT 결과를 저장할 이미지 (캔버스)
        createUniformBuffers();            // RT용 카메라/조명 정보 UBO
        createBottomLevelAS();             // 모델 형상 가속 구조 (BLAS) 빌드
        createObjDescriptionBuffer();      // 버텍스/인덱스 버퍼 주소 정보
        createTopLevelAS();                // 인스턴스 배치 가속 구조 (TLAS) 빌드

        // =================================================================
        // 7. 레이 트레이싱 파이프라인 (Ray Tracing Pipeline)
        // =================================================================
        createRTDescriptorSetLayout();     // RT 쉐이더 입력 구조 정의
        createRTDescriptorPool();          // RT용 디스크립터 풀
        createRTDescriptorSets();          // 리소스(AS, StorageImage, 등) 연결
        createRTPipeline();                // RayGen, Miss, Hit 쉐이더 파이프라인 생성
        createShaderBindingTable();        // 쉐이더 함수 포인터 테이블(SBT) 생성

        // =================================================================
        // 8. 실행 명령 녹화 및 동기화 (Recording & Sync)
        // =================================================================
        // 실제 GPU가 수행할 작업을 기록합니다.
        createCommandBuffers();            // Compute -> Raster -> RT 순서로 명령 녹화
        createSyncObjects();               // 세마포어 및 펜스 생성 (GPU-CPU 동기화)

        // =================================================================
        // 9. 디버깅 및 도구 (Tools)
        // =================================================================
        // 프로파일러 초기화 (Query Pool 생성 등) - Device 생성 이후여야 함
        profiler.init(device, physicalDevice);
    }

    /*void setupScene() {

        objects.push_back({ "models/PiggyBank.obj", glm::vec3(0.0f, 5.5f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.8f, 0.8f, 0.8f), glm::vec3(0.3f, 0.2f, 0.15f) });

        lights.resize(1);
        lights[0] = { glm::vec3(0.0f, 10.0f, 0.0f), 1.0f, glm::vec3(1.0f, 1.0f, 1.0f), 1 };

        std::cout << "Scene: " << objects.size() << " objects, " << lights.size() << " lights" << std::endl;

    }*/

    void setupScene() {
        objects.clear();


        // 바닥 (Floor) - 밝은 회색
        objects.push_back({
            "models/cube.obj",
            glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(20.0f, 0.1f, 20.0f),
            glm::vec3(0.8f, 0.8f, 0.8f)
            ,false
            });

        objects.push_back({
            "models/cube.obj",
            glm::vec3(0.0f, 12.0f, 0.0f),
            glm::vec3(0.0f),
            glm::vec3(20.0f, 0.1f, 20.0f),
            glm::vec3(1.0f, 1.0f, 1.0f)
            ,false
            });


        objects.push_back({
            "models/cube.obj",
            glm::vec3(0.0f, 6.0f, -10.0f),
            glm::vec3(0.0f),
            glm::vec3(20.0f, 10.0f, 0.1f),
            glm::vec3(0.9f, 0.9f, 0.9f),
            false
            });


        objects.push_back({
            "models/cube.obj",
            glm::vec3(-10.0f, 6.0f, 0.0f),
            glm::vec3(0.0f),
            glm::vec3(0.1f, 10.0f, 20.0f),
            glm::vec3(0.8f, 0.1f, 0.1f),
            false
            });

        objects.push_back({
            "models/cube.obj",
            glm::vec3(10.0f, 6.0f, 0.0f),
            glm::vec3(0.0f),
            glm::vec3(0.1f, 10.0f, 20.0f),
            glm::vec3(0.1f, 0.8f, 0.1f),
            false
            });

        objects.push_back({
            "models/table.obj",
            glm::vec3(0.0f, -0.9f, 0.0f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.5f, 0.5f, 0.5f),
            glm::vec3(0.55f, 0.27f, 0.07f),
            false
            });

        objects.push_back({
            "models/chair.obj",
            glm::vec3(0.0f, -0.9f, 2.5f),
            glm::vec3(0.0f, 180.0f, 0.0f),
            glm::vec3(0.6f, 0.6f, 0.6f),
            glm::vec3(0.2f, 0.2f, 0.6f),
            false
            });

        objects.push_back({
            "models/chair.obj",
            glm::vec3(-3.5f, -0.9f, 0.0f),
            glm::vec3(0.0f, -70.0f, 0.0f),
            glm::vec3(0.6f, 0.6f, 0.6f),
            glm::vec3(0.2f, 0.2f, 0.6f),
            false
            });

        objects.push_back({
            "models/PiggyBank.obj",
            glm::vec3(9.0f, 1.95f, 0.0f),
            glm::vec3(0.0f, -30.0f, 0.0f),
            glm::vec3(0.6f, 0.6f, 0.6f),
            glm::vec3(1.0f, 0.4f, 0.2f),
            true
            });

        objects.push_back({
            "models/PiggyBank.obj",
            glm::vec3(-2.0f, 1.95f, 0.0f),
            glm::vec3(0.0f, -30.0f, 0.0f),
            glm::vec3(0.6f, 0.6f, 0.6f),
            glm::vec3(1.0f, 0.2f, 0.2f),
            true
            });

        objects.push_back({
            "models/cube.obj",
            glm::vec3(1.5f, 2.1f, 0.5f),
            glm::vec3(0.0f, 45.0f, 0.0f),
            glm::vec3(0.2f, 0.2f, 0.2f),
            glm::vec3(1.0f, 0.8f, 0.0f),
            false
            });

        lights.resize(2);

        /*lights[0] = {
            glm::vec3(0.0f, 9.0f, 2.0f),
            1.2f,
            glm::vec3(1.0f, 1.0f, 1.0f),
            1
        };*/

        lights[0] = {
            glm::vec3(-8.0f, 5.0f, -5.0f),
            0.5f,
            glm::vec3(0.0f, 0.0f, 0.9f),
            1
        };

        std::cout << "Scene Loaded: " << objects.size() << " objects, " << lights.size() << " lights" << std::endl;

    }

    void processInput() {
        float cameraSpeed = camera.speed * deltaTime;


        if (keys[GLFW_KEY_W]) camera.position += cameraSpeed * camera.front;
        if (keys[GLFW_KEY_S]) camera.position -= cameraSpeed * camera.front;
        if (keys[GLFW_KEY_A]) camera.position -= glm::normalize(glm::cross(camera.front, camera.up)) * cameraSpeed;
        if (keys[GLFW_KEY_D]) camera.position += glm::normalize(glm::cross(camera.front, camera.up)) * cameraSpeed;


        if (keys[GLFW_KEY_E]) camera.position += cameraSpeed * camera.up;
        if (keys[GLFW_KEY_Q]) camera.position -= cameraSpeed * camera.up;
    }

    void mainLoop() {
        while (!glfwWindowShouldClose(window)) {
            float currentFrame = glfwGetTime();
            deltaTime = currentFrame - lastFrame;
            lastFrame = currentFrame;
            glfwPollEvents();

            processInput();

            drawFrame();
        }
        vkDeviceWaitIdle(device);
    }

    void drawFrame() {
        vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("failed to acquire swap chain image!");
        }

        updateUniformBuffer(currentFrame);

        updateRasterUniformBuffer(currentFrame); // [추가] Raster용 업데이트

        vkResetFences(device, 1, &inFlightFences[currentFrame]);
        vkResetCommandBuffer(commandBuffers[currentFrame], 0);
        recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers[currentFrame];
        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
            throw std::runtime_error("failed to submit draw command buffer!");
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(presentQueue, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapChain();
        }
        else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present swap chain image!");
        }

        // [추가] 6. 결과 확인 및 윈도우 타이틀 업데이트 (0.5초마다)
        titleUpdateTimer += deltaTime; // deltaTime은 mainLoop에서 계산됨
        if (titleUpdateTimer > 0.5f) {
            std::string stats = profiler.getResultsString();

            if (!stats.empty()) {
                std::string title = "P.R.I.S.M Hybrid Renderer - " + stats;
                glfwSetWindowTitle(window, title.c_str());
            }
            titleUpdateTimer = 0.0f;
        }


        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void updateUniformBuffer(uint32_t currentImage) {
        UniformBufferObject ubo{};
        glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float)swapChainExtent.height, 0.1f, 100.0f);
        proj[1][1] *= -1;
        ubo.viewInverse = glm::inverse(view);
        ubo.projInverse = glm::inverse(proj);
        ubo.cameraPos = camera.position;
        ubo.lightCount = lights.size();
        for (size_t i = 0; i < lights.size(); i++) {
            ubo.lights[i] = lights[i];

            if (!isLightOn) {
                ubo.lights[i].enabled = 0;
            }
            else {
                ubo.lights[i].enabled = 1;
            }
        }
        memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }


    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        // [추가] 5-1. 프레임 측정 시작
        profiler.beginFrame(commandBuffer);

        // ==========================================================================================
        // Phase 0: Compute Simulation (물리 연산) - 가장 먼저 실행!
        // ==========================================================================================
        profiler.beginSection(commandBuffer, "0. Compute Sim");

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &computeDescriptorSet, 0, nullptr);

        struct ComputePush { float dt; float time; int count; } push;
        push.dt = 0.016f; // deltaTime (임시 고정값, 실제로는 mainLoop에서 받은 deltaTime 사용 권장)
        push.time = (float)glfwGetTime();
        push.count = (int)objects.size();

        vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        // 워크그룹 계산: (개수 + 255) / 256
        vkCmdDispatch(commandBuffer, (uint32_t)(objects.size() + 255) / 256, 1, 1);

        profiler.endSection(commandBuffer);

        // [중요] Memory Barrier: Compute(쓰기) -> Vertex/RT(읽기) 동기화
        VkBufferMemoryBarrier computeBarrier{};
        computeBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        computeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; // Compute가 씀
        computeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR; // Vertex/RT가 읽음
        computeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        computeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        computeBarrier.buffer = objectSSBO;
        computeBarrier.offset = 0;
        computeBarrier.size = VK_WHOLE_SIZE;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // 소스 단계
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, // 목적 단계
            0, 0, nullptr,
            1, &computeBarrier,
            0, nullptr);

        // ==========================================================================================
        // Phase 1: Rasterization Pass (G-Buffer/Depth 생성)
        // ==========================================================================================

        // [추가] 5-2. 래스터화 구간 시작
        profiler.beginSection(commandBuffer, "1. Raster");

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = swapChainExtent;

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[1].depthStencil = { 1.0f, 0 };

        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        // 래스터화 시작
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelineLayout, 0, 1, &rasterDescriptorSets[currentFrame], 0, nullptr);

        // isRaster == true 인 물체만 그리기
        for (size_t i = 0; i < objects.size(); i++) {
            if (objects[i].isRaster) {
                VkBuffer vertexBuffers[] = { geometryDataList[i].vertexBuffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, geometryDataList[i].indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                RasterPushConstant pushConst{};
                pushConst.model = glm::mat4(1.0f);
                pushConst.model = glm::translate(pushConst.model, objects[i].position);
                pushConst.model = glm::rotate(pushConst.model, glm::radians(objects[i].rotation.x), glm::vec3(1, 0, 0));
                pushConst.model = glm::rotate(pushConst.model, glm::radians(objects[i].rotation.y), glm::vec3(0, 1, 0));
                pushConst.model = glm::rotate(pushConst.model, glm::radians(objects[i].rotation.z), glm::vec3(0, 0, 1));
                pushConst.model = glm::scale(pushConst.model, objects[i].scale);
                pushConst.color = objects[i].color;

                vkCmdPushConstants(commandBuffer, graphicsPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RasterPushConstant), &pushConst);
                vkCmdDrawIndexed(commandBuffer, geometryDataList[i].indexCount, 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(commandBuffer);

        // [추가] 5-3. 래스터화 구간 종료
        profiler.endSection(commandBuffer);

        // ==========================================================================================
        // Phase 2: Copy Background (Swapchain -> Storage Image)
        // 래스터화된 결과를 RT 캔버스(Storage Image)로 복사해옵니다. (배경 합성용)
        // ==========================================================================================

        // 1. Swapchain: Present Src -> Transfer Src (복사 원본 준비)
        VkImageMemoryBarrier swapChainReadBarrier{};
        swapChainReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapChainReadBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // RenderPass 종료 후 상태
        swapChainReadBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        swapChainReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapChainReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapChainReadBarrier.image = swapChainImages[imageIndex];
        swapChainReadBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        swapChainReadBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        swapChainReadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        // 2. Storage Image: Undefined -> Transfer Dst (복사 타겟 준비)
        VkImageMemoryBarrier storageWriteBarrier{};
        storageWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        storageWriteBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        storageWriteBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        storageWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageWriteBarrier.image = storageImage;
        storageWriteBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        storageWriteBarrier.srcAccessMask = 0;
        storageWriteBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        // 3. Depth Image: Depth Attach -> Shader Read (RT에서 읽기 준비)
        VkImageMemoryBarrier depthBarrier{};
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = depthImage;
        depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkImageMemoryBarrier preCopyBarriers[] = { swapChainReadBarrier, storageWriteBarrier, depthBarrier };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 3, preCopyBarriers);

        // [복사 실행] 래스터화된 화면을 Storage Image로 복사
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copyRegion.extent = { swapChainExtent.width, swapChainExtent.height, 1 };
        vkCmdCopyImage(commandBuffer, swapChainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // ==========================================================================================
        // Phase 3: Ray Tracing Pass
        // ==========================================================================================

        // [추가] 5-4. 레이 트레이싱 구간 시작
        profiler.beginSection(commandBuffer, "2. RayTrace");

        // Storage Image: Transfer Dst -> General (RT 쉐이더 쓰기 준비)
        VkImageMemoryBarrier storageGeneralBarrier{};
        storageGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        storageGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        storageGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        storageGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageGeneralBarrier.image = storageImage;
        storageGeneralBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        storageGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        storageGeneralBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &storageGeneralBarrier);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipelineLayout, 0, 1, &rtDescriptorSets[currentFrame], 0, nullptr);

        auto vkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
        vkCmdTraceRaysKHR(commandBuffer, &raygenRegion, &missRegion, &hitRegion, &callableRegion, swapChainExtent.width, swapChainExtent.height, 1);


        // [추가] 5-5. 레이 트레이싱 구간 종료
        profiler.endSection(commandBuffer);


        // ==========================================================================================
        // Phase 4: Final Copy (Storage Image -> Swapchain)
        // RT 결과(Storage)를 다시 화면(Swapchain)으로 복사
        // ==========================================================================================

        // 1. Storage Image: General -> Transfer Src
        VkImageMemoryBarrier copySrcBarrier{};
        copySrcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copySrcBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        copySrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copySrcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copySrcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copySrcBarrier.image = storageImage;
        copySrcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        copySrcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        copySrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        // 2. Swapchain: Transfer Src -> Transfer Dst (Phase 2에서 Src였음)
        VkImageMemoryBarrier copyDstBarrier{};
        copyDstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copyDstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // 아까 배경 복사할 때 Src였음
        copyDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyDstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyDstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyDstBarrier.image = swapChainImages[imageIndex];
        copyDstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        copyDstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copyDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier postRTBarriers[] = { copySrcBarrier, copyDstBarrier };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, postRTBarriers);

        // 최종 복사 실행
        vkCmdCopyImage(commandBuffer, storageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapChainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // ==========================================================================================
        // Phase 5: Present 준비
        // ==========================================================================================

        VkImageMemoryBarrier presentBarrier{};
        presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.image = swapChainImages[imageIndex];
        presentBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        presentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        presentBarrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }
    }

    void createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("validation layers requested, but not available!");
        }
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Ray Traced Scene";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        auto extensions = getRequiredExtensions();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = &debugCreateInfo;
        }
        else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create instance!");
        }
    }

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
    }

    void setupDebugMessenger() {
        if (!enableValidationLayers) return;
        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        populateDebugMessengerCreateInfo(createInfo);
        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            throw std::runtime_error("failed to set up debug messenger!");
        }
    }

    void createSurface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("failed to find GPUs with Vulkan support!");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& dev : devices) {
            if (isDeviceSuitable(dev)) {
                physicalDevice = dev;
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) throw std::runtime_error("failed to find a suitable GPU!");
    }

    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = { indices.graphicsFamily.value(), indices.presentFamily.value() };

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
        bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
        rtPipelineFeatures.pNext = &bufferDeviceAddressFeatures;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;
        asFeatures.pNext = &rtPipelineFeatures;

        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
        descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
        descriptorIndexingFeatures.pNext = &asFeatures;

        // 1. 기본 기능 구조체 설정
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.shaderInt64 = VK_TRUE;   // 64비트 정수 활성화
        deviceFeatures.shaderFloat64 = VK_TRUE; // 64비트 실수 활성화 (필요하다면)

        VkPhysicalDeviceFeatures2 deviceFeatures2{};
        deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        deviceFeatures2.features = deviceFeatures;
        deviceFeatures2.pNext = &descriptorIndexingFeatures;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pNext = &deviceFeatures2;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        //추가됨
        createInfo.pEnabledFeatures = nullptr;

        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
        }
        else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
            throw std::runtime_error("failed to create logical device!");
        }

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    void createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);
        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };

        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            throw std::runtime_error("failed to create swap chain!");
        }

        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());
        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
    }

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create image views!");
            }
        }
    }

    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create command pool!");
        }
    }


    void createStorageImage() {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { swapChainExtent.width, swapChainExtent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = swapChainImageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &storageImage) != VK_SUCCESS) {
            throw std::runtime_error("failed to create storage image!");
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, storageImage, &memRequirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &storageImageMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate storage image memory!");
        }
        vkBindImageMemory(device, storageImage, storageImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = storageImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapChainImageFormat;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &viewInfo, nullptr, &storageImageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create storage image view!");
        }

        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = storageImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        endSingleTimeCommands(commandBuffer);
    }

    void createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);
        uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        uniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
        uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uniformBuffers[i], uniformBuffersMemory[i]);
            vkMapMemory(device, uniformBuffersMemory[i], 0, bufferSize, 0, &uniformBuffersMapped[i]);
        }
    }

    void createInstanceColorBuffer() {
        struct Color4 { float r, g, b, a; };

        std::vector<Color4> colors;
        colors.reserve(objects.size());
        for (auto& obj : objects) {
            colors.push_back({ obj.color.r, obj.color.g, obj.color.b, 1.0f });
        }

        VkDeviceSize sz = sizeof(Color4) * colors.size();

        createBuffer(sz,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            instanceColorBuffer, instanceColorMemory);

        void* data;
        vkMapMemory(device, instanceColorMemory, 0, sz, 0, &data);
        memcpy(data, colors.data(), sz);
        vkUnmapMemory(device, instanceColorMemory);
    }



    void createBottomLevelAS() {
        auto vkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
        auto vkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
        auto vkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
        auto vkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");

        bottomLevelAS.resize(objects.size());
        geometryDataList.resize(objects.size());

        for (size_t i = 0; i < objects.size(); i++) {
            geometryDataList[i] = loadGeometry(objects[i].modelPath, objects[i].scale);

            VkAccelerationStructureGeometryKHR geometry{};
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            geometry.geometry.triangles.vertexData.deviceAddress = getBufferDeviceAddress(geometryDataList[i].vertexBuffer);
            geometry.geometry.triangles.vertexStride = sizeof(Vertex);
            geometry.geometry.triangles.maxVertex = geometryDataList[i].vertexCount;
            geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            geometry.geometry.triangles.indexData.deviceAddress = getBufferDeviceAddress(geometryDataList[i].indexBuffer);

            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
            buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount = 1;
            buildInfo.pGeometries = &geometry;

            uint32_t primitiveCount = geometryDataList[i].indexCount / 3;
            VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
            sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

            createBuffer(sizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, geometryDataList[i].blasBuffer, geometryDataList[i].blasMemory);

            VkAccelerationStructureCreateInfoKHR createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            createInfo.buffer = geometryDataList[i].blasBuffer;
            createInfo.size = sizeInfo.accelerationStructureSize;
            createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &geometryDataList[i].blas) != VK_SUCCESS) {
                throw std::runtime_error("failed to create BLAS!");
            }
            bottomLevelAS[i] = geometryDataList[i].blas;

            VkBuffer scratchBuffer;
            VkDeviceMemory scratchMemory;
            createBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory);

            buildInfo.dstAccelerationStructure = geometryDataList[i].blas;
            buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratchBuffer);

            VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
            buildRangeInfo.primitiveCount = primitiveCount;
            buildRangeInfo.primitiveOffset = 0;
            buildRangeInfo.firstVertex = 0;
            buildRangeInfo.transformOffset = 0;
            const VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfo = &buildRangeInfo;

            VkCommandBuffer commandBuffer = beginSingleTimeCommands();
            vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pBuildRangeInfo);
            endSingleTimeCommands(commandBuffer);

            vkDestroyBuffer(device, scratchBuffer, nullptr);
            vkFreeMemory(device, scratchMemory, nullptr);
        }
        std::cout << "Created " << bottomLevelAS.size() << " Bottom Level AS" << std::endl;
    }

    void createObjDescriptionBuffer() {
        std::vector<ObjDesc> objDescs;
        objDescs.reserve(objects.size());

        for (const auto& geoData : geometryDataList) {
            ObjDesc desc;
            desc.vertexAddress = getBufferDeviceAddress(geoData.vertexBuffer);
            desc.indexAddress = getBufferDeviceAddress(geoData.indexBuffer);
            objDescs.push_back(desc);
        }

        VkDeviceSize bufferSize = sizeof(ObjDesc) * objDescs.size();

        createBuffer(bufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            objDescBuffer, objDescBufferMemory);

        void* data;
        vkMapMemory(device, objDescBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, objDescs.data(), bufferSize);
        vkUnmapMemory(device, objDescBufferMemory);
    }

    void createTopLevelAS() {
        auto vkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
        auto vkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
        auto vkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
        auto vkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        for (size_t i = 0; i < objects.size(); i++) {
            // if (objects[i].isRaster) continue;

            VkAccelerationStructureInstanceKHR instance{};
            glm::mat4 transform = glm::mat4(1.0f);
            transform = glm::translate(transform, objects[i].position);
            transform = glm::rotate(transform, glm::radians(objects[i].rotation.x), glm::vec3(1, 0, 0));
            transform = glm::rotate(transform, glm::radians(objects[i].rotation.y), glm::vec3(0, 1, 0));
            transform = glm::rotate(transform, glm::radians(objects[i].rotation.z), glm::vec3(0, 0, 1));
            transform = glm::scale(transform, objects[i].scale);
            glm::mat4 transposed = glm::transpose(transform);
            memcpy(&instance.transform, &transposed, sizeof(VkTransformMatrixKHR));

            instance.instanceCustomIndex = i;

            //instance.mask = 0xFF;
            // [수정 및 추가 코드] 인스턴스 마스크 설정
            // 0x01: RT 물체 (직접 보여야 함)
            // 0x02: Raster 물체 (그림자/반사에만 보여야 함)
            if (objects[i].isRaster) {
                instance.mask = 0x02;
            }
            else {
                instance.mask = 0x01;
            }
            
            instance.instanceShaderBindingTableRecordOffset = 0;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addressInfo.accelerationStructure = bottomLevelAS[i];
            instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
            instances.push_back(instance);
        }

        VkDeviceSize instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * instances.size();
        createBuffer(instanceBufferSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, instanceBuffer, instanceMemory);

        void* data;
        vkMapMemory(device, instanceMemory, 0, instanceBufferSize, 0, &data);
        memcpy(data, instances.data(), instanceBufferSize);
        vkUnmapMemory(device, instanceMemory);

        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = getBufferDeviceAddress(instanceBuffer);

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        uint32_t primitiveCount = instances.size();
        VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
        sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

        createBuffer(sizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tlasBuffer.buffer, tlasBuffer.memory);

        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = tlasBuffer.buffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &topLevelAS) != VK_SUCCESS) {
            throw std::runtime_error("failed to create TLAS!");
        }

        VkBuffer scratchBuffer;
        VkDeviceMemory scratchMemory;
        createBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory);

        buildInfo.dstAccelerationStructure = topLevelAS;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratchBuffer);

        VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
        buildRangeInfo.primitiveCount = primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfo = &buildRangeInfo;

        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pBuildRangeInfo);
        endSingleTimeCommands(commandBuffer);

        vkDestroyBuffer(device, scratchBuffer, nullptr);
        vkFreeMemory(device, scratchMemory, nullptr);
        std::cout << "Created Top Level AS with " << instances.size() << " instances" << std::endl;
    }

    void createRTDescriptorSetLayout() {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

        // [추가] Binding 5: Depth Sampler (Combined Image Sampler)
        VkDescriptorSetLayoutBinding depthBinding{};
        depthBinding.binding = 5;
        depthBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depthBinding.descriptorCount = 1;
        depthBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        bindings[5] = depthBinding;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rtDescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create RT descriptor set layout!");
        }
    }

    void createRTDescriptorPool() {
        std::array<VkDescriptorPoolSize, 6> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[2].descriptorCount = MAX_FRAMES_IN_FLIGHT;
        poolSizes[3] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT };
        poolSizes[4] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT };
        
        poolSizes[5].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[5].descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &rtDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create RT descriptor pool!");
        }
    }

    void createRTDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, rtDescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = rtDescriptorPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts = layouts.data();

        rtDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
        if (vkAllocateDescriptorSets(device, &allocInfo, rtDescriptorSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate RT descriptor sets!");
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkWriteDescriptorSetAccelerationStructureKHR descASInfo{};
            descASInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            descASInfo.accelerationStructureCount = 1;
            descASInfo.pAccelerationStructures = &topLevelAS;

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageView = storageImageView;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkDescriptorBufferInfo colorBufInfo{};
            colorBufInfo.buffer = instanceColorBuffer;
            colorBufInfo.offset = 0;
            colorBufInfo.range = VK_WHOLE_SIZE;

            VkDescriptorBufferInfo objDescInfo{};
            objDescInfo.buffer = objDescBuffer;
            objDescInfo.offset = 0;
            objDescInfo.range = VK_WHOLE_SIZE;

            // [추가] Depth Image Info 설정
            VkDescriptorImageInfo depthImageInfo{};
            depthImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // 읽기 전용 레이아웃
            depthImageInfo.imageView = depthImageView;
            depthImageInfo.sampler = depthSampler;

            std::array<VkWriteDescriptorSet, 6> descriptorWrites{};
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = rtDescriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pNext = &descASInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = rtDescriptorSets[i];
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pImageInfo = &imageInfo;

            descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2].dstSet = rtDescriptorSets[i];
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[2].descriptorCount = 1;
            descriptorWrites[2].pBufferInfo = &bufferInfo;

            descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[3].dstSet = rtDescriptorSets[i];
            descriptorWrites[3].dstBinding = 3;
            descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[3].descriptorCount = 1;
            descriptorWrites[3].pBufferInfo = &colorBufInfo;


            descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[4].dstSet = rtDescriptorSets[i];
            descriptorWrites[4].dstBinding = 4;
            descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[4].descriptorCount = 1;
            descriptorWrites[4].pBufferInfo = &objDescInfo;

            // [추가] Descriptor Write 설정
            VkWriteDescriptorSet depthWrite{};
            depthWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            depthWrite.dstSet = rtDescriptorSets[i];
            depthWrite.dstBinding = 5;
            depthWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            depthWrite.descriptorCount = 1;
            depthWrite.pImageInfo = &depthImageInfo;
            descriptorWrites[5] = depthWrite;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        }
    }

    void createRTPipeline() {
        auto raygenCode = readFile("shaders/raygen.rgen.spv");
        auto missCode = readFile("shaders/miss.rmiss.spv");
        auto shadowMissCode = readFile("shaders/shadow.rmiss.spv");
        auto chitCode = readFile("shaders/closesthit.rchit.spv");

        VkShaderModule raygenModule = createShaderModule(raygenCode);
        VkShaderModule missModule = createShaderModule(missCode);
        VkShaderModule shadowMissModule = createShaderModule(shadowMissCode);
        VkShaderModule chitModule = createShaderModule(chitCode);

        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
        VkPipelineShaderStageCreateInfo raygenStage{};
        raygenStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        raygenStage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        raygenStage.module = raygenModule;
        raygenStage.pName = "main";
        shaderStages.push_back(raygenStage);

        VkPipelineShaderStageCreateInfo missStage{};
        missStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        missStage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        missStage.module = missModule;
        missStage.pName = "main";
        shaderStages.push_back(missStage);

        VkPipelineShaderStageCreateInfo shadowMissStage{};
        shadowMissStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shadowMissStage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        shadowMissStage.module = shadowMissModule;
        shadowMissStage.pName = "main";
        shaderStages.push_back(shadowMissStage);

        VkPipelineShaderStageCreateInfo chitStage{};
        chitStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        chitStage.stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        chitStage.module = chitModule;
        chitStage.pName = "main";
        shaderStages.push_back(chitStage);

        std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups;
        VkRayTracingShaderGroupCreateInfoKHR raygenGroup{};
        raygenGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        raygenGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        raygenGroup.generalShader = 0;
        raygenGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
        raygenGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
        raygenGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
        shaderGroups.push_back(raygenGroup);

        VkRayTracingShaderGroupCreateInfoKHR missGroup{};
        missGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        missGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        missGroup.generalShader = 1;
        missGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
        missGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
        missGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
        shaderGroups.push_back(missGroup);

        VkRayTracingShaderGroupCreateInfoKHR shadowMissGroup{};
        shadowMissGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        shadowMissGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        shadowMissGroup.generalShader = 2;
        shadowMissGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
        shadowMissGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
        shadowMissGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
        shaderGroups.push_back(shadowMissGroup);

        VkRayTracingShaderGroupCreateInfoKHR hitGroup{};
        hitGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        hitGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        hitGroup.generalShader = VK_SHADER_UNUSED_KHR;
        hitGroup.closestHitShader = 3;
        hitGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
        hitGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
        shaderGroups.push_back(hitGroup);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &rtDescriptorSetLayout;
        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &rtPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create RT pipeline layout!");
        }

        VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.groupCount = static_cast<uint32_t>(shaderGroups.size());
        pipelineInfo.pGroups = shaderGroups.data();
        pipelineInfo.maxPipelineRayRecursionDepth = 2;
        pipelineInfo.layout = rtPipelineLayout;

        auto vkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR");
        if (vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rtPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create RT pipeline!");
        }

        vkDestroyShaderModule(device, raygenModule, nullptr);
        vkDestroyShaderModule(device, missModule, nullptr);
        vkDestroyShaderModule(device, shadowMissModule, nullptr);
        vkDestroyShaderModule(device, chitModule, nullptr);
    }


    uint32_t alignUp(uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    void createShaderBindingTable() {
        auto vkGetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR");

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
        rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 deviceProperties2{};
        deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        deviceProperties2.pNext = &rtProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProperties2);

        const uint32_t handleSize = rtProperties.shaderGroupHandleSize;
        const uint32_t handleAlignment = rtProperties.shaderGroupHandleAlignment;
        const uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);
        const uint32_t groupCount = 4;
        const uint32_t sbtSize = groupCount * handleSizeAligned;

        std::vector<uint8_t> shaderHandleStorage(sbtSize);
        if (vkGetRayTracingShaderGroupHandlesKHR(device, rtPipeline, 0, groupCount, sbtSize, shaderHandleStorage.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to get RT shader group handles!");
        }

        VkDeviceSize raygenSize = handleSizeAligned;
        VkDeviceSize missSize = handleSizeAligned * 2;
        VkDeviceSize hitSize = handleSizeAligned;

        createBuffer(raygenSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, raygenShaderBindingTable, raygenShaderBindingTableMemory);
        createBuffer(missSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, missShaderBindingTable, missShaderBindingTableMemory);
        createBuffer(hitSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, hitShaderBindingTable, hitShaderBindingTableMemory);

        void* data;
        vkMapMemory(device, raygenShaderBindingTableMemory, 0, raygenSize, 0, &data);
        memcpy(data, shaderHandleStorage.data(), handleSize);
        vkUnmapMemory(device, raygenShaderBindingTableMemory);

        vkMapMemory(device, missShaderBindingTableMemory, 0, missSize, 0, &data);
        memcpy(data, shaderHandleStorage.data() + handleSizeAligned, handleSize);
        memcpy((uint8_t*)data + handleSizeAligned, shaderHandleStorage.data() + handleSizeAligned * 2, handleSize);
        vkUnmapMemory(device, missShaderBindingTableMemory);

        vkMapMemory(device, hitShaderBindingTableMemory, 0, hitSize, 0, &data);
        memcpy(data, shaderHandleStorage.data() + handleSizeAligned * 3, handleSize);
        vkUnmapMemory(device, hitShaderBindingTableMemory);

        raygenRegion.deviceAddress = getBufferDeviceAddress(raygenShaderBindingTable);
        raygenRegion.stride = handleSizeAligned;
        raygenRegion.size = handleSizeAligned;

        missRegion.deviceAddress = getBufferDeviceAddress(missShaderBindingTable);
        missRegion.stride = handleSizeAligned;
        missRegion.size = missSize;

        hitRegion.deviceAddress = getBufferDeviceAddress(hitShaderBindingTable);
        hitRegion.stride = handleSizeAligned;
        hitRegion.size = handleSizeAligned;
    }

    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        auto vkGetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR");
        return vkGetBufferDeviceAddressKHR(device, &info);
    }

    void createCommandBuffers() {
        commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
        if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate command buffers!");
        }
    }

    void createSyncObjects() {
        imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create sync objects!");
            }
        }
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create buffer!");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            allocInfo.pNext = &allocFlagsInfo;
        }

        if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate buffer memory!");
        }
        vkBindBufferMemory(device, buffer, bufferMemory, 0);
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("failed to find suitable memory type!");
    }

    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        vkEndCommandBuffer(commandBuffer);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shader module!");
        }
        return shaderModule;
    }

    static std::vector<char> readFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("failed to open file: " + filename);
        }
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) break;
            i++;
        }
        return indices;
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        QueueFamilyIndices indices = findQueueFamilies(device);
        bool extensionsSupported = checkDeviceExtensionSupport(device);
        bool swapChainAdequate = false;
        if (extensionsSupported) {
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
            swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }
        return indices.isComplete() && extensionsSupported && swapChainAdequate;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());
        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }
        return requiredExtensions.empty();
    }

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
        SwapChainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
        }
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }
        return availableFormats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        else {
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            VkExtent2D actualExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }

    std::vector<const char*> getRequiredExtensions() {
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    bool checkValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        for (const char* layerName : validationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }
            if (!layerFound) return false;
        }
        return true;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
        std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
        return VK_FALSE;
    }

    void recreateSwapChain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }
        vkDeviceWaitIdle(device);
        cleanupSwapChain();
        createSwapChain();
        createImageViews();

        // --- [수정] 순서 중요! ---
        createDepthResources(); // Depth 재생성
        createFramebuffers();   // Framebuffer 재생성 (RenderPass는 유지)
        // -----------------------


        createStorageImage();
        createRTDescriptorSets();
    }

    void cleanupSwapChain() {
        vkDestroyImageView(device, storageImageView, nullptr);
        vkDestroyImage(device, storageImage, nullptr);
        vkFreeMemory(device, storageImageMemory, nullptr);

        // --- [수정] 프레임버퍼 및 Depth 해제 추가 ---
        vkDestroyImageView(device, depthImageView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthImageMemory, nullptr);

        for (auto framebuffer : swapChainFramebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        // -------------------------------------------


        for (auto imageView : swapChainImageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        vkDestroySwapchainKHR(device, swapChain, nullptr);
    }

    void cleanup() {
        // [추가] 4. 프로파일러 정리
        profiler.cleanup();

        cleanupSwapChain();

        // --- [수정] 래스터화 리소스 해제 추가 ---
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, graphicsPipelineLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        // --------------------------------------


        vkDestroyPipeline(device, rtPipeline, nullptr);
        vkDestroyPipelineLayout(device, rtPipelineLayout, nullptr);
        vkDestroyBuffer(device, raygenShaderBindingTable, nullptr);
        vkFreeMemory(device, raygenShaderBindingTableMemory, nullptr);
        vkDestroyBuffer(device, missShaderBindingTable, nullptr);
        vkFreeMemory(device, missShaderBindingTableMemory, nullptr);
        vkDestroyBuffer(device, hitShaderBindingTable, nullptr);
        vkFreeMemory(device, hitShaderBindingTableMemory, nullptr);
        vkDestroyBuffer(device, instanceColorBuffer, nullptr);
        vkFreeMemory(device, instanceColorMemory, nullptr);
        vkDestroyBuffer(device, objDescBuffer, nullptr);
        vkFreeMemory(device, objDescBufferMemory, nullptr);

        // [추가] 래스터화 UBO 및 디스크립터 해제
        vkDestroyDescriptorPool(device, rasterDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, rasterDescriptorSetLayout, nullptr);
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroyBuffer(device, rasterUniformBuffers[i], nullptr);
            vkFreeMemory(device, rasterUniformBuffersMemory[i], nullptr);
        }

        vkDestroySampler(device, depthSampler, nullptr);


        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroyBuffer(device, uniformBuffers[i], nullptr);
            vkFreeMemory(device, uniformBuffersMemory[i], nullptr);
        }

        vkDestroyDescriptorPool(device, rtDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, rtDescriptorSetLayout, nullptr);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(device, commandPool, nullptr);

        auto vkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
        for (auto& geoData : geometryDataList) {
            vkDestroyBuffer(device, geoData.vertexBuffer, nullptr);
            vkFreeMemory(device, geoData.vertexMemory, nullptr);
            vkDestroyBuffer(device, geoData.indexBuffer, nullptr);
            vkFreeMemory(device, geoData.indexMemory, nullptr);
            vkDestroyBuffer(device, geoData.blasBuffer, nullptr);
            vkFreeMemory(device, geoData.blasMemory, nullptr);
            if (geoData.blas != VK_NULL_HANDLE) {
                vkDestroyAccelerationStructureKHR(device, geoData.blas, nullptr);
            }
        }

        if (topLevelAS != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(device, topLevelAS, nullptr);
        }
        vkDestroyBuffer(device, tlasBuffer.buffer, nullptr);
        vkFreeMemory(device, tlasBuffer.memory, nullptr);
        vkDestroyBuffer(device, instanceBuffer, nullptr);
        vkFreeMemory(device, instanceMemory, nullptr);

        vkDestroyDevice(device, nullptr);

        if (enableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }

        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    // [추가] 1. 깊이 버퍼(Depth Buffer) 생성
    void createDepthResources() {
        VkFormat depthFormat = findDepthFormat();
        // [수정] VK_IMAGE_USAGE_SAMPLED_BIT 추가
        createImage(swapChainExtent.width, swapChainExtent.height, depthFormat,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory);
        
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &depthImageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create depth image view!");
        }
    }

    // [추가] 1-1. 지원되는 깊이 포맷 찾기 헬퍼 함수
    VkFormat findDepthFormat() {
        return findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
        );
    }

    // [추가] 1-2. 포맷 지원 여부 확인 헬퍼 함수
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
                return format;
            } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
                return format;
            }
        }
        throw std::runtime_error("failed to find supported format!");
    }

    // [추가] 1-3. 이미지 생성 헬퍼 (기존 createStorageImage 등에서 사용 가능하도록 일반화)
    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            throw std::runtime_error("failed to create image!");
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate image memory!");
        }

        vkBindImageMemory(device, image, imageMemory, 0);
    }

    // [추가] 2. 렌더 패스(Render Pass) 생성
    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        // 중요: 레이 트레이싱 결과 위에 그릴 것이므로 로드(Load) 시 지우지 않고(LOAD) 내용을 유지합니다.
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // 레이 트레이싱 후 복사된 상태
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = findDepthFormat();
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create render pass!");
        }
    }

    // [추가] 3. 프레임버퍼(Framebuffer) 생성
    void createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());

        for (size_t i = 0; i < swapChainImageViews.size(); i++) {
            std::array<VkImageView, 2> attachments = {
                swapChainImageViews[i],
                depthImageView
            };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create framebuffer!");
            }
        }
    }

    // [추가] 4. 그래픽스 파이프라인(Graphics Pipeline) 생성
    void createGraphicsPipeline() {
        auto vertShaderCode = readFile("shaders/raster.vert.spv"); // 파일 필요!
        auto fragShaderCode = readFile("shaders/raster.frag.spv"); // 파일 필요!

        VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
        VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        // Vertex Input (기존 Vertex 구조체 사용)
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT; // pos
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT; // normal
        attributeDescriptions[1].offset = offsetof(Vertex, normal);

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)swapChainExtent.width;
        viewport.height = (float)swapChainExtent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // [수정] Push Constant 범위 정의 (Model Matrix 등 전달)
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(RasterPushConstant);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        //pipelineLayoutInfo.setLayoutCount = 0; // 필요시 디스크립터 레이아웃 추가
        //pipelineLayoutInfo.pushConstantRangeCount = 0;
        // 
        // [수정] Descriptor Set Layout 연결 (View/Proj UBO)
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &rasterDescriptorSetLayout;

        // [수정] Push Constant 연결
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &graphicsPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline layout!");
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil; // 깊이 스텐실 추가
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = graphicsPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
    }

    // [추가] 래스터화용 디스크립터 셋 레이아웃 생성 (UBO: View/Proj)
    void createRasterDescriptorSetLayout() {
        std::vector<VkDescriptorSetLayoutBinding> bindings(2); // 1개 -> 2개로 변경

        // Binding 0: Raster UBO (View, Proj)
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        // [추가] Binding 1: Object SSBO (Compute가 업데이트한 위치 정보)
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; // 버텍스 쉐이더에서 읽음

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rasterDescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create raster descriptor set layout!");
        }
    }

    // [추가] 래스터화용 UBO 버퍼 생성
    void createRasterUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(RasterUBO);

        rasterUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        rasterUniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
        rasterUniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rasterUniformBuffers[i], rasterUniformBuffersMemory[i]);
            vkMapMemory(device, rasterUniformBuffersMemory[i], 0, bufferSize, 0, &rasterUniformBuffersMapped[i]);
        }
    }

    // [추가] 래스터화용 디스크립터 풀 생성
    void createRasterDescriptorPool() {
        std::array<VkDescriptorPoolSize, 2> poolSizes{};

        // 1. Uniform Buffer (기존)
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        // 2. Storage Buffer (추가)
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &rasterDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create raster descriptor pool!");
        }
    }

    // [추가] 래스터화용 디스크립터 셋 할당 및 업데이트
    void createRasterDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, rasterDescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = rasterDescriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocInfo.pSetLayouts = layouts.data();

        rasterDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
        if (vkAllocateDescriptorSets(device, &allocInfo, rasterDescriptorSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate raster descriptor sets!");
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            // 1. UBO 정보 (Binding 0)
            VkDescriptorBufferInfo uboInfo{};
            uboInfo.buffer = rasterUniformBuffers[i];
            uboInfo.offset = 0;
            uboInfo.range = sizeof(RasterUBO);

            // 2. SSBO 정보 (Binding 1) - [추가]
            VkDescriptorBufferInfo ssboInfo{};
            ssboInfo.buffer = objectSSBO; // Compute가 쓰는 그 버퍼!
            ssboInfo.offset = 0;
            ssboInfo.range = VK_WHOLE_SIZE;

            std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

            // Binding 0 쓰기
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = rasterDescriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &uboInfo;

            // Binding 1 쓰기 (SSBO 연결)
            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = rasterDescriptorSets[i];
            descriptorWrites[1].dstBinding = 1; // 쉐이더의 binding = 1 과 일치
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pBufferInfo = &ssboInfo;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        }
    }

    // [추가] 매 프레임 래스터화 UBO 업데이트 (카메라 이동 반영)
    void updateRasterUniformBuffer(uint32_t currentImage) {
        RasterUBO ubo{};
        ubo.view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
        ubo.proj = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float)swapChainExtent.height, 0.1f, 100.0f);
        ubo.proj[1][1] *= -1; // Vulkan Y 좌표계 반전

        memcpy(rasterUniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }

    // [추가] 초기화 함수 (initVulkan에서 호출 필요)
    void createDepthSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST; // 깊이값은 보간하면 안 되므로 NEAREST
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

        if (vkCreateSampler(device, &samplerInfo, nullptr, &depthSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create texture sampler!");
        }
    }

    
    void createObjectSSBO() {
        VkDeviceSize bufferSize = sizeof(ObjState) * objects.size();

        // 1. Staging Buffer 생성 (CPU에서 데이터를 만들어서 잠깐 담을 곳)
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(bufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingBufferMemory);

        // 2. 초기 데이터 생성 (램덤 속도 부여 등)
        std::vector<ObjState> initialStates(objects.size());
        for (size_t i = 0; i < objects.size(); i++) {
            initialStates[i].model = glm::mat4(1.0f);

            // 기존 setupScene에서 설정한 위치를 가져옴
            initialStates[i].model = glm::translate(glm::mat4(1.0f), objects[i].position);
            initialStates[i].model = glm::scale(initialStates[i].model, objects[i].scale);
            // 회전은 복잡하니 일단 생략하거나 초기값 적용

            initialStates[i].position = glm::vec4(objects[i].position, 1.0f);

            // 랜덤 속도 부여 (예: -2.0 ~ 2.0 사이)
            float vx = ((rand() % 100) / 25.0f) - 2.0f;
            float vy = ((rand() % 100) / 25.0f) - 2.0f;
            float vz = ((rand() % 100) / 25.0f) - 2.0f;
            initialStates[i].velocity = glm::vec4(vx, vy, vz, 0.0f);

            initialStates[i].color = glm::vec4(objects[i].color, 1.0f);
        }

        // 3. 데이터 복사 (Map -> Memcpy -> Unmap)
        void* data;
        vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, initialStates.data(), (size_t)bufferSize);
        vkUnmapMemory(device, stagingBufferMemory);

        // 4. 실제 SSBO 생성 (GPU 전용 메모리)
        // 용도: Compute가 쓰고(STORAGE), Vertex가 읽고(STORAGE or VERTEX), 전송받음(TRANSFER_DST)
        createBuffer(bufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            objectSSBO, objectSSBOMemory);

        // 5. Staging -> SSBO 복사 명령 실행
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(commandBuffer, stagingBuffer, objectSSBO, 1, &copyRegion);
        endSingleTimeCommands(commandBuffer);

        // 6. Staging Buffer 제거
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingBufferMemory, nullptr);

        std::cout << "Created Object SSBO for " << objects.size() << " objects." << std::endl;
    }

    void createComputePipeline() {
        // =================================================================
        // 1. Descriptor Set Layout (SSBO 바인딩 0번)
        // =================================================================
        VkDescriptorSetLayoutBinding ssboBinding{};
        ssboBinding.binding = 0;
        ssboBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssboBinding.descriptorCount = 1;
        // Compute 뿐만 아니라 Vertex(Raster), RayTracing(ClosestHit)에서도 읽을 수 있게 공유
        ssboBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &ssboBinding;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &computeDescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute descriptor set layout!");
        }

        // =================================================================
        // 2. Pipeline Layout (Push Constant: dt, time, count)
        // =================================================================
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(float) * 2 + sizeof(int); // 12 bytes

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &computeDescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &computePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute pipeline layout!");
        }

        // =================================================================
        // 3. Compute Pipeline 생성
        // =================================================================
        auto computeShaderCode = readFile("shaders/simulation.comp.spv"); // 쉐이더 파일 필요
        VkShaderModule computeShaderModule = createShaderModule(computeShaderCode);

        VkPipelineShaderStageCreateInfo shaderStageInfo{};
        shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStageInfo.module = computeShaderModule;
        shaderStageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = shaderStageInfo;
        pipelineInfo.layout = computePipelineLayout;

        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute pipeline!");
        }

        vkDestroyShaderModule(device, computeShaderModule, nullptr);

        // =================================================================
        // 4. Descriptor Pool & Set 할당 및 업데이트
        // =================================================================
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &computeDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute descriptor pool!");
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = computeDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &computeDescriptorSetLayout;

        if (vkAllocateDescriptorSets(device, &allocInfo, &computeDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate compute descriptor set!");
        }

        // SSBO 연결
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = objectSSBO;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = computeDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }


};

int main() {
    RayTracedScene app;
    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


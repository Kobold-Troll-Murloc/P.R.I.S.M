# 📘 Ogre-Next Vulkan 아키텍처 가이드 (PRISM 프로젝트)

이 문서는 Ogre-Next 엔진의 Vulkan 렌더링 시스템 이해 및 수정을 위한 핵심 구조 기술서임.

## 1. 핵심 클래스 계층 구조 (Core Classes)

| 클래스명 | 역할 및 설명 | 관련 파일 |
| :--- | :--- | :--- |
| **`VulkanRenderSystem`** | 렌더 시스템 엔트리 포인트. 고수준 명령을 Vulkan API로 변환 및 리소스 생명 주기 관리 | `OgreVulkanRenderSystem.h/cpp` |
| **`VulkanDevice`** | `VkDevice`, `VkQueue` 관리. 커맨드 버퍼 제출(Submit) 및 GPU 동기화(Stall) 담당 | `OgreVulkanDevice.h/cpp` |
| **`VulkanQueue`** | **핵심 클래스.** Vulkan 상태(Encoder State) 및 커맨드 버퍼 기록 흐름 관리 | `OgreVulkanQueue.h/cpp` |
| **`VulkanCache`** | `VkRenderPass`, `VkPipeline` 등 고비용 객체 캐싱 및 재사용을 통한 성능 최적화 | `OgreVulkanCache.h/cpp` |
| **`VulkanVaoManager`** | GPU 메모리 할당 및 버퍼(Vertex, Index, Const) 배치 관리 | `OgreVulkanVaoManager.h/cpp` |

### 1.1 클래스별 상세 기능 및 주요 함수 (실례 및 연결점 포함)

#### **1) VulkanVaoManager (메모리 설계)**
- **상세**: Vulkan의 복잡한 메모리 모델을 관리하며 고성능 버퍼 접근 전략 제공.
- **주요 함수**: `createConstBuffer()`, `createVertexBuffer()`, `allocateVao()`
- **박스 예시 [1단계]**: 박스의 8개 꼭짓점 데이터를 GPU 메모리의 최적 위치에 할당 및 저장.
- **오거 연결**: **`Mesh` / `Item` → `VaoManager`**. 정점/인덱스 데이터의 GPU 메모리 동기화 담당.

#### **2) VulkanCache (data 관리자)**
- **상세**: 중복된 Vulkan 객체 생성을 방지하여 CPU 오버헤드 감소.
- **주요 함수**: `getRenderPass()`, `getGraphicsPipeline()`
- **박스 예시 [2단계]**: 재질 설정을 확인하여 기존에 생성된 최적화된 `VkPipeline`(PSO) 확보.
- **오거 연결**: **`Hlms` (Material) → `VulkanCache`**. 고수준 재질 설정을 Vulkan PSO로 변환.

#### **3) VulkanQueue (주 작업)**
- **상세**: 커맨드 버퍼 기록의 핵심 상태 머신. 작업 유형 간 전환 시 배리어 자동화.
- **주요 함수**: `getGraphicsEncoder()`, `getComputeEncoder()`, `endAllEncoders()`
- **박스 예시 [3단계]**: 명령 기록 전 커맨드 버퍼를 '그래픽 모드(Render Pass)'로 전환.
- **오거 연결**: **`Compositor` → `VulkanQueue`**. 렌더링 순서에 따른 엔코더 상태 및 패스 제어.

#### **4) VulkanRenderSystem (실행 지휘)**
- **상세**: Ogre 엔진의 추상화된 명령을 실제 Vulkan 리소스와 파이프라인으로 연결.
- **주요 함수**: `_render()`, `_setPipelineStateObject()`, `bindGpuProgramParameters()`
- **박스 예시 [4단계]**: `vkCmdDraw`를 통해 실제 박스 그리기 명령을 커맨드 버퍼에 기록.
- **오거 연결**: **`SceneManager` → `VulkanRenderSystem`**. 가시성 판단 후 실제 Draw Call 명령 생성.

#### **5) VulkanDevice (하드웨어 연결점)**
- **상세**: 물리적 GPU와의 통신 채널. 명령 제출의 최종 단계 제어.
- **주요 함수**: `commitAndNextCommandBuffer()`, `stall()`, `createDevice()`
- **박스 예시 [5단계]**: 작성된 커맨드 버퍼를 GPU에 제출(`Submit`)하여 화면에 출력.
- **오거 연결**: **`Root::renderOneFrame` → `VulkanDevice`**. 프레임 결과물 최종 출력.

---

## 2. Encoder 상태 머신 (VulkanQueue)

### 2.1 상세 원리 및 주요 함수
- **상세**: Vulkan 작업(그래픽, 컴퓨트, 복사) 간의 배타적 구간을 관리하며, 상태 전환 시 동기화 배리어(Barrier)를 자동 생성하여 데이터 무결성을 보장함.
- **주요 함수**:
    - `getGraphicsEncoder()`: `vkCmdBeginRenderPass`를 통해 그래픽 그리기 구간 시작.
    - `getCopyEncoder()`: 리소스 업로드 및 GPU 간 데이터 복사 구간 시작.
    - `endAllEncoders()`: 모든 활성 구간을 닫고 명령 기록 확정.

### 2.2 실례 및 연결점
- **박스 예시**: 박스 데이터 업로드(`Copy`) 후 그리기(`Graphics`) 시, `VulkanQueue`가 중간에 배리어를 삽입하여 업로드가 끝난 뒤에 그리도록 제어함.
- **오거 연결**: **`Compositor` → `VulkanQueue`**. 그림자 맵 생성과 메인 렌더링 사이의 이미지 레이아웃 전환 등을 자동으로 처리함.

---

## 3. 리소스 바인딩 및 Descriptor 관리

### 3.1 상세 원리 및 주요 함수
- **상세**: 쉐이더가 사용할 텍스처와 버퍼를 연결하는 방식. Ogre-Next는 성능을 위해 매번 바인딩하지 않고, 변경된 리소스만 업데이트하는 **'Dirty Checking'** 방식을 사용함.
- **주요 함수**:
    - `_setTexture()`: 특정 슬롯에 사용할 텍스처를 지정하고 상태를 '더티(Dirty)'로 표시.
    - `bindGpuProgramParameters()`: 쉐이더 파라미터(Uniform)를 전용 버퍼에 기록.
    - `_updateDescriptorSets()`: Draw Call 직전, 변경된 리소스들만 한꺼번에 `vkUpdateDescriptorSets`로 갱신.

### 3.2 실례 및 연결점
- **박스 예시**: 박스에 입힐 "나무 질감 텍스처"와 "회전 행렬 데이터"를 각각 쉐이더의 0번, 1번 슬롯에 연결함.
- **오거 연결**: **`HlmsDatablock` ↔ `Descriptor Set`**. 오거 재질의 속성들이 Vulkan의 Descriptor Set 인덱스로 1:1 매핑되어 전달됨.

---

## 4. 프레임 렌더링 흐름 (Life Cycle)

### 4.1 상세 원리 및 주요 함수
- **상세**: 한 프레임이 시작되어 화면에 출력되기까지의 전 과정. 멀티버퍼링(In-flight Frames)을 통해 CPU와 GPU가 병렬로 일할 수 있게 설계됨.
- **멀티 패스 구조**: 하나의 프레임은 여러 개의 독립적인 `Render Pass`들의 집합으로 구성됨. (예: 그림자 패스 -> 불투명 패스 -> 투명 패스 -> UI 패스)
- **주요 함수**:
    - `_beginFrame()`: 이번 프레임에 사용할 커맨드 버퍼와 펜스(Fence)를 확보.
    - `beginRenderPassDescriptor()`: 실제 그리기 영역을 설정하고 배경색을 지움(Clear). 각 개별 패스가 시작될 때마다 호출됨.
    - `_endFrame()` / `commitAndNextCommandBuffer()`: 기록 완료 후 GPU에 제출 및 화면 출력 요청.

### 4.2 실례 및 연결점
- **박스 예시**: 
    1. **그림자 패스**: 박스의 그림자 데이터를 그림자용 텍스처에 기록.
    2. **메인 패스**: 그림자 데이터를 참조하여 박스 본체를 화면에 그림.
- **오거 연결**: **`Compositor` ↔ `Multi-Pass`**. 오거의 컴포지터 노드 설정에 따라 Vulkan의 `Render Pass`가 순차적으로 실행되며, 패스 간 동기화는 `VulkanQueue`가 담당함.

---

## 5. 시스템 확장을 위한 가이드 (Extension Points)

### 5.1 상세 원리 및 수정 포인트
- **상세**: 기본 기능을 넘어 레이트레이싱이나 커스텀 포스트 프로세싱을 추가하기 위한 지점.
- **주요 수정 파일**:
    - `OgreVulkanDevice.cpp`: 신규 확장 기능(예: `VK_KHR_ray_tracing_pipeline`) 활성화.
    - `OgreVulkanRenderSystem.cpp`: 새로운 명령(예: `vkCmdTraceRaysKHR`)을 실행하는 함수 추가.
    - `OgreVulkanQueue.cpp`: 신규 작업 유형에 대응하는 커스텀 엔코더 상태 추가.

### 5.2 실례 및 연결점
- **박스 예시**: 박스에 실시간 레이트레이싱 그림자를 넣고 싶다면, `VulkanDevice`에서 확장을 켜고 `RenderSystem`에서 광선 추적 명령을 내리도록 수정함.
- **오거 연결**: **`Custom Hlms` / `Compositor Pass`**. 오거에서 새로운 패스 타입을 정의하고, 이를 Vulkan의 확장 기능 호출부와 연결하여 기능을 완성함.

---

## 6. 종합 멘탈 모델 (Device-Descriptor-Pass-Queue)

Vulkan 시스템의 전체 작동 원리를 공장(Factory) 시스템에 비유하여 요약함.

### 6.1 공장 시스템 비유
- **VulkanDevice (공장)**: 하드웨어 그 자체이며, 모든 작업이 일어나는 기반 시설임.
- **RenderPassDescriptor (작업 지시서)**: 어떤 도화지에 어떻게 그릴지 적은 계획서임.
- **VkRenderPass (조립 라인)**: 지시서에 따라 공장 내에 설치된 실제 공정임.
- **VulkanQueue (컨베이어 벨트)**: 작성된 명령서를 실어 나르고 실제로 실행하는 통로임.

### 6.2 시스템의 핵심 이점
- **병렬 처리 (Multi-Queue)**: 자재 운반과 조립 작업의 동시 수행이 가능함.
- **결론**: 전체 시스템은 **"공장 가동 → 지시서 작성 → 라인 배치 → 벨트 가동"**의 순서로 맞물려 돌아감.

---

## 7. 하이브리드 파이프라인 설계 (Rasterization + Ray Tracing)

재귀적 광선 추적(Refraction, Reflection)을 포함한 고수준 레이트레이싱 통합 전략임.

### 7.1 엔코더 기반 통합 (Encoder-based Integration)
- **핵심 전략**: 래스터화 패스를 완전히 종료한 후, **독립적인 Ray Tracing 엔코더**로 전환하여 광선을 추적함.
- **이유**: 재귀적 광선 대응 및 파이프라인 상태의 완전한 격리를 통한 안정성 확보.

### 7.2 구현 단계별 수정 포인트
1.  **`VulkanDevice`**: 레이트레이싱 하드웨어 가속 확장 활성화.
2.  **`VulkanQueue`**: `getRayTracingEncoder()` 메서드 추가 및 배리어 로직 구현.
3.  **`VulkanRenderSystem`**: 가속 구조 바인딩 및 `TraceRays` 명령 수행 로직 구현.
4.  **`Compositor`**: G-Buffer와 레이트레이싱 결과를 결합하는 하이브리드 시나리오 구성.

---

## 8. 엔진 수정 가이드 (Engine Modification Guide)

하이브리드 파이프라인 구현을 위해 수정해야 할 오거 엔진(Ogre-Next) 소스 코드 포인트임.

### 8.1 Vulkan 렌더 시스템 플러그인 (`RenderSystems/Vulkan`)
- **`OgreVulkanDevice.cpp`**: `VK_KHR_ray_tracing_pipeline` 등 확장 기능 활성화 로직 추가.
- **`OgreVulkanQueue.h/cpp`**: `EncoderState` 열거형에 `EncoderRayTracingOpen` 추가 및 `getRayTracingEncoder()` 구현.
- **`OgreVulkanRenderSystem.h/cpp`**: 가속 구조(AS) 관리 변수 추가 및 `vkCmdTraceRaysKHR` 호출 함수 구현.
- **`OgreVulkanVaoManager.cpp`**: 정점 데이터를 기반으로 가속 구조(AS)를 생성/업데이트하는 로직 추가.

### 8.2 엔진 코어 및 워크플로우 (`OgreMain` / `Compositor`)
- **`OgreCompositorPass.h`**: 새로운 패스 타입(`PASS_RAYTRACING`) 정의.
- **`OgreCompositorManager2.cpp`**: 레이트레이싱 패스를 생성하고 실행할 수 있도록 컴포지터 매니저 확장.
- **`OgreHlms.cpp`**: 레이트레이싱 전용 쉐이더(Ray Gen, Hit, Miss)와 쉐이더 바인딩 테이블(SBT) 관리 로직 연동.

### 8.3 수정 전략
- **단계적 접근**: `VulkanDevice`의 확장 기능 활성화를 우선 수행하여 하드웨어 지원 여부를 확인한 후, 점진적으로 엔코더와 패스 시스템을 확장해 나감.

---

## 9. 상세 학습 가이드 (Deep Dive Learning Guide)

PRISM 프로젝트의 하이브리드 파이프라인 완성도를 높이기 위한 주제별 상세 학습 자료 및 가이드임.

### 9.1 Vulkan 동기화와 배리어 (Synchronization & Barriers)
Vulkan의 가장 어려운 산이자 성능의 핵심임. GPU는 수천 개의 코어가 병렬로 돌아가므로, "쓰기 작업이 끝난 뒤 읽기"를 명시적으로 지시해야 함.

*   **핵심 개념**: `Pipeline Barrier`, `Stage Flags` (어느 단계에서?), `Access Flags` (어떤 작업을?), `Image Layout Transition`.
*   **학습 자료**:
    *   [Vulkan Guide - Synchronization](https://vkguide.dev/docs/extra-chapter/vulkan_synchronization_refresher/): 동기화 개념을 가장 쉽게 설명한 사이트.
    *   [Khronos Synchronization Examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples): 흔한 실수와 올바른 배리어 예시 모음.
    *   [Maister's Graphics Blog (Sync)](https://themaister.net/blog/2019/08/14/yet-another-vulkan-synchronization-tutorial/): 고급 사용자를 위한 깊이 있는 분석.
*   **Ogre-Next 실습**: `OgreVulkanQueue.cpp`의 `insertBarrier` 함수가 어떻게 상태 변화를 감지하고 배리어를 치는지 분석할 것.

### 9.2 GPU 메모리 관리와 VMA (Memory Management)
단순한 `malloc`이 아님. GPU 메모리는 한정되어 있고 접근 속도가 다르므로 효율적인 배치가 중요함.

*   **핵심 개념**: `Device Local` vs `Host Visible`, `Memory Aliasing`, `Sub-allocation`.
*   **학습 자료**:
    *   [Vulkan Memory Allocator (VMA) Docs](https://gpuopen-librariesandsdk.github.io/VulkanMemoryAllocator/html/): 업계 표준 라이브러리인 VMA의 작동 원리.
    *   [NVIDIA: Vulkan Memory Management](https://developer.nvidia.com/vulkan-memory-management): 하드웨어 제조사 관점의 메모리 최적화.
*   **Ogre-Next 실습**: `OgreVulkanVaoManager.cpp`에서 `VMA`를 활용해 어떻게 대규모 버퍼를 관리하는지 확인.

### 9.3 레이트레이싱 하드웨어 가속 (RTX Internals)
광선과 삼각형의 충돌을 어떻게 초당 수억 번 계산하는지에 대한 기술적 이해.

*   **핵심 개념**: `BLAS/TLAS`(가속 구조), `SBT`(Shader Binding Table), `Ray Generation/Closest Hit/Miss Shader`.
*   **학습 자료**:
    *   [Vulkan Tutorial - Ray Tracing](https://vulkan-tutorial.com/): 기본 튜토리얼 (확장판 필요).
    *   [NVIDIA Vulkan Ray Tracing Tutorial](https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/): 실제 코드로 배우는 RTX 구현의 정석.
    *   [Ray Tracing Gems (Free PDF)](https://www.realtimerendering.com/raytracinggems/): 이론과 실무 기법의 집대성.
*   **Ogre-Next 실습**: `OgreVulkanRenderSystem.cpp`에 `vkCmdTraceRaysKHR`를 호출하기 위한 구조 설계 시작.

### 9.4 하이브리드 디노이징 (Denoising)
레이트레이싱의 노이즈를 지우는 마법. 픽셀당 1개 광선(1spp)으로도 사진 같은 품질을 내는 핵심 기술.

*   **핵심 개념**: `Temporal Reprojection` (이전 프레임 활용), `Bilateral Filter` (경계 보존 필터), `ASVGF`.
*   **학습 자료**:
    *   [Spatiotemporal Variance Guided Filtering (SVGF) Paper](https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced): 가장 유명한 디노이징 논문.
    *   [PBR Book - Light Transport](https://pbr-book.org/3ed-2018/Light_Transport_I_Surface_Reflection): 빛의 물리적 거동 이해.
*   **PRISM 적용**: G-Buffer(Normal, Depth) 정보를 활용해 레이트레이싱 결과의 노이즈를 뭉개지 않고 선명하게 제거하는 로직 구현.

### 9.5 Ogre-Next HLMS (High Level Material System)
오거만의 강력한 재질 관리자. 수천 개의 쉐이더 조합을 자동으로 관리함.

*   **핵심 개념**: `Syntax Highlighter` (JSON/C++ 템플릿), `Property Map`, `Piece Files`.
*   **학습 자료**:
    *   [Ogre-Next Manual (HLMS Section)](https://ogrecave.github.io/ogre-next/api/2.3/hlms.html): 공식 문서.
    *   [Ogre Forum: HLMS Tutorial](https://forums.ogre3d.org/viewtopic.php?t=82337): 커뮤니티의 실질적인 가이드.
*   **Ogre-Next 실습**: `Samples/Media/Hlms/Pbs/Any/` 경로의 `.hlms` 파일들을 열어 템플릿 언어의 구조를 파악할 것.

### 9.6 프로파일링 및 디버깅 (Tools)
눈에 보이지 않는 GPU 내부를 들여다보는 도구들.

*   **추천 도구**:
    *   **RenderDoc**: 특정 프레임을 캡처해 모든 API 호출과 리소스를 전수 조사.
    *   **NVIDIA Nsight Graphics**: 실시간 GPU 부하, 유닛별 병목 지점 확인.
    *   **Vulkan Validation Layers**: API 오사용을 실시간으로 경고해 주는 필수 레이어.
*   **학습 방법**: 직접 만든 코드를 RenderDoc으로 캡처해 `VulkanQueue`가 의도한 대로 배리어를 치는지 눈으로 확인할 것.

---
**💡 학습 팁**: 한꺼번에 다 하려 하지 마세요. **"Vulkan 동기화(9.1)"**와 **"레이트레이싱 기본(9.3)"**을 병행하며 PRISM의 첫 번째 Ray를 쏘아 올리는 것을 첫 목표로 잡으세요!

---

## 10. 오거-넥스트 엔진 개조 전략 (Breaking the Abstraction)

오거-넥스트는 강력한 추상화를 제공하지만, 우리가 원하는 하이브리드 기능을 넣으려면 이 추상화 계층을 명확히 이해하고 "어디를 뚫어야 할지" 결정해야 함.

### 10.1 엔진 데이터 흐름 역추적 (Tracing the Flow)
1.  **SceneManager**: "무엇을 그릴 것인가?" (Scene Graph, Frustum Culling)
2.  **Compositor**: "어떤 순서로 그릴 것인가?" (Passes, Render Targets)
3.  **RenderSystem (Vulkan)**: "어떻게 Vulkan으로 번역할 것인가?" (API Calls)
4.  **Hlms (Pbs/Unlit)**: "어떤 쉐이더와 리소스를 쓸 것인가?" (PSO, Descriptor Sets)

### 10.2 하이브리드 렌더러 삽입 지점 (Injection Points)
*   **지점 A: `VulkanQueue` 엔코더 확장**: 래스터화(Graphics)와 레이트레이싱(RayTracing) 사이의 컨텍스트 스위칭을 관리함.
*   **지점 B: `VaoManager` 데이터 추출**: 기존 정점 버퍼(`Ogre::VertexBufferPacked`)에서 레이트레이싱용 BLAS를 빌드하기 위한 데이터를 뽑아내야 함.
*   **지점 C: `CompositorPass` 신규 정의**: 오거의 스크립트(.compositor)에서 `pass raytracing { ... }` 명령을 인식하도록 엔진 코어 확장.

---

## 11. PRISM 하이브리드 파이프라인 구현 로드맵 (Roadmap)

공부와 구현을 동시에 병행하기 위한 5단계 실무 로드맵임.

### [1단계] Vulkan RT 확장 기능 활성화 (Infrastructure)
*   **목표**: `VulkanDevice`에서 레이트레이싱 하드웨어 가속 기능을 켜고, 엔진이 이를 인식하게 함.
*   **파악할 파일**: `OgreVulkanDevice.cpp` 내의 `VkPhysicalDeviceFeatures2` 설정부.
*   **학습 포인트**: `Extension`과 `Feature`의 차이점, `pNext` 체인을 통한 구조체 전달 방식.

### [2단계] 가속 구조(AS) 빌더 구현 (Data Bridge)
*   **목표**: 오거의 `Mesh` 데이터를 불칸의 `Acceleration Structure`로 변환.
*   **파악할 파일**: `OgreVulkanVaoManager.cpp`.
*   **학습 포인트**: `vkCreateAccelerationStructureKHR`, 정점 버퍼의 레이아웃 해석 및 GPU 메모리 복사.

### [3단계] 레이트레이싱 전용 엔코더 추가 (Command Flow)
*   **목표**: `VulkanQueue에 getRayTracingEncoder()`를 추가하여 RT 명령 전용 구간 생성.
*   **파악할 파일**: `OgreVulkanQueue.h/cpp`.
*   **학습 포인트**: `vkCmdTraceRaysKHR` 호출 전후의 이미지 레이아웃(General Layout) 전환 및 배리어 설정.

### [4단계] 하이브리드 컴포지터 패스 (Workflow Integration)
*   **목표**: G-Buffer(래스터화) 생성 후, 그 결과물을 쉐이더에 입력으로 넣어 광선을 추적.
*   **파악할 파일**: `OgreCompositorPass.cpp`, `OgreVulkanRenderSystem.cpp`.
*   **학습 포인트**: `Descriptor Set`을 통해 G-Buffer 텍스처를 RT 쉐이더에 바인딩하는 방법.

### [5단계] 디노이징 및 최종 합성 (Post-Processing)
*   **목표**: RT 노이즈를 제거하고 메인 화면에 래스터화 결과와 섞어 출력.
*   **파악할 파일**: `OgreHlms.cpp` 및 커스텀 HLMS 쉐이더(.hlms).
*   **학습 포인트**: 시간적/공간적 필터링(Temporal/Spatial Filtering) 기법의 쉐이더 구현.

---

## 12. HLMS (High Level Material System) 심층 분석

HLMS는 오거-넥스트의 고성능을 지탱하는 핵심 엔진으로, 수많은 재질 옵션(텍스처 유무, 조명 개수, 하이브리드 RT 설정 등)에 대응하는 최적화된 쉐이더 코드를 자동으로 생성함.

### 12.1 HLMS의 핵심 철학: "The Shader Factory"
과거에는 개발자가 모든 쉐이더 파일(.glsl, .hlsl)을 직접 관리했지만, HLMS는 **C++ 속성(Properties)**과 **쉐이더 템플릿(Pieces)**을 결합하여 필요한 쉐이더를 즉석에서 '조립'함.

*   **HlmsManager**: 모든 HLMS 구현체(`Pbs`, `Unlit` 등)를 총괄 관리.
*   **HlmsDatablock**: 특정 물체에 적용되는 구체적인 재질 값(Diffuse Color, Roughness 등)을 담은 객체.
*   **HlmsProperty**: C++에서 쉐이더로 전달되는 '스위치'. (예: `pbs_texture_diffuse=1`)
*   **HlmsPiece**: 쉐이더 코드의 '부품'. 조건문(`@property`, `@piece`)을 통해 코드에 포함되거나 제외됨.

### 12.2 쉐이더 생성 흐름 (Shader Generation Flow)
1.  **C++ Property 설정**: `HlmsPbs::calculateHash`에서 현재 재질 상태를 분석해 수많은 `Int` 속성을 설정함.
2.  **해시(Hash) 계산**: 설정된 속성들을 조합해 고유한 번호(Hash)를 생성. 동일한 설정은 이 해시를 통해 재사용됨.
3.  **템플릿 파싱**: `Samples/Media/Hlms/Pbs/Any/`에 있는 `.hlms` 파일들을 읽어와 조건에 맞는 코드만 남김.
4.  **컴파일**: 최종 생성된 소스 코드를 Vulkan SPIR-V로 컴파일하여 GPU에 로드.

### 12.3 HLMS와 레이트레이싱의 결합 (PRISM 전략)
레이트레이싱을 구현할 때 HLMS를 어떻게 활용해야 하는가?
*   **전략**: 기존 `HlmsPbs`를 상속받거나 확장하여 **"Closest Hit Shader"** 템플릿을 만듦.
*   **이점**: 레이트레이싱 광선이 물체에 부딪혔을 때, 기존 래스터화에서 쓰던 PBR 재질 계산 로직(BRDF)을 그대로 재사용하여 시각적 일관성을 확보할 수 있음.

### 12.4 추천 실습 및 분석 파일
*   **C++ 핵심 로직**: `Components/Hlms/Pbs/src/OgreHlmsPbs.cpp`
    *   `calculateHash()` 함수를 보며 어떻게 수십 개의 속성이 결정되는지 분석할 것.
*   **쉐이더 템플릿**: `Samples/Media/Hlms/Pbs/Any/Main/VertexShader_vs.hlms` (또는 `PixelShader_ps.hlms`)
    *   `@property`, `@piece`, `@insert` 구문이 실제 코드에서 어떻게 동작하는지 파악할 것.
*   **데이터 바인딩**: `Components/Hlms/Pbs/src/OgreHlmsPbsDatablock.cpp`
    *   사용자의 입력값이 어떻게 쉐이더 상수로 변환되는지 확인.

### 12.5 학습 자료 (HLMS 특화)
*   **[Ogre-Next 공식 매뉴얼 - HLMS](https://ogrecave.github.io/ogre-next/api/2.3/hlms.html)**: 가장 정확하지만 방대한 문서. "How HLMS works" 섹션부터 정독 권장.
*   **[Hlms Template Syntax Guide](https://ogrecave.github.io/ogre-next/api/2.3/_h_l_m_s_template_language.html)**: `@property` 같은 문법의 상세 명세서.
*   **[Ogre Forum: HLMS for Beginners](https://forums.ogre3d.org/viewtopic.php?t=82337)**: 커뮤니티 전문가들이 설명하는 HLMS 도입 가이드.

---

## 13. G-Buffer 설계 (The Foundation of Hybrid)

하이브리드 렌더링에서 래스터화는 레이트레이싱을 위한 **"가이드 맵"**인 G-Buffer를 생성하는 역할을 수행함.

### 13.1 G-Buffer의 구성 요소
레이트레이싱 패스에서 효율적인 광선 추적과 디노이징을 위해 다음 정보들이 필요함:
*   **World Space Normal (RGB16F)**: 반사 광선의 방향 계산용.
*   **Depth / World Position (R32F)**: 광선 시작점(Ray Origin) 결정용.
*   **Roughness / Metallic (RG8)**: 광선의 분산(Roughness) 정도 결정용.
*   **Motion Vectors (RG16F)**: 이전 프레임 데이터를 재활용하는 **디노이징(Temporal Reprojection)**의 핵심.

### 13.2 Ogre-Next에서의 MRT(Multiple Render Targets) 구현
*   **Compositor 설정**: `.compositor` 파일에서 `target` 노드를 정의하고, `render_pass`에서 여러 개의 텍스처를 동시에 쓰는 MRT 설정을 활성화해야 함.
*   **HLMS 수정**: G-Buffer 패스용 HLMS 쉐이더(`PixelShader_ps.hlms`)에서 `outColour0`, `outColour1` 등으로 각 정보를 출력하도록 `@property` 분기 처리.

---

## 14. RTX 프레임 수명 주기 통합 (Integration Flow)

오거의 프레임 루프 어느 시점에 레이트레이싱 명령을 찔러 넣어야 하는가?

### 14.1 이상적인 하이브리드 시퀀스
1.  **[래스터화] G-Buffer Pass**: 화면의 기하학적 정보 추출.
2.  **[불칸] Barrier**: G-Buffer 텍스처를 `Color Attachment`에서 `Shader Read (General/ReadOnly)` 레이아웃으로 전환.
3.  **[레이트레이싱] Ray Tracing Pass**: G-Buffer를 읽어 반사/그림자/전역 조명(GI) 계산.
4.  **[불칸] Barrier**: RT 결과물을 읽기 가능한 상태로 전환.
5.  **[래스터화] Composition Pass**: 기존 라이팅 결과와 RT 결과를 섞어 최종 화면 생성.

### 14.2 구현 핵심: `CompositorPass` 커스텀화
*   `OgreCompositorPass.h`에서 `PASS_CUSTOM`을 활용하거나, 아예 새로운 `PASS_RAYTRACING` 타입을 엔진에 등록하여 오거의 컴포지터 시스템이 자연스럽게 RT 명령을 수행하게 만듦.

---

## 15. SBT(Shader Binding Table) 전략 (The Bridge)

SBT는 **"어떤 물체에 부딪혔을 때, 어떤 쉐이더를 실행할지"** 결정하는 Vulkan 전용 데이터 구조임.

### 15.1 오거 데이터와 SBT의 매핑
*   **Ogre Item/Mesh ID** ↔ **SBT Index**: 오거의 모든 `Item`은 고유한 `HlmsDatablock`을 가짐. 이 데이터블록의 인덱스를 SBT의 인덱스와 1:1 매핑하거나, SBT 내부에 인덱싱 정보를 포함시켜야 함.
*   **Hit Group 구성**: 
    *   `Closest Hit`: PBR 재질 계산 로직 포함.
    *   `Any Hit`: 투명도(Alpha Testing/Blending) 처리를 위해 필요.

### 15.2 동적 업데이트 (Dynamic Scenes)
*   카메라가 움직이거나 물체가 이동할 때, 오거의 `SceneManager`가 관리하는 트랜스폼 데이터를 Vulkan의 **TLAS(Top Level Acceleration Structure)**에 실시간으로 동기화하는 로직이 필수적임.

---

## 16. 동적 가속 구조 관리 (Dynamic AS Management)

게임 씬의 물체들은 끊임없이 이동함. 레이트레이싱은 이 변화를 **가속 구조(AS) 업데이트**를 통해 실시간으로 반영해야 함.

### 16.1 Rebuild vs Refit (성능의 핵심)
*   **Rebuild**: 가속 구조를 처음부터 다시 빌드함. 최상의 광선 추적 효율을 내지만 CPU/GPU 부하가 큼.
*   **Refit**: 기존 구조의 박스 크기만 조정함. 빌드 속도는 매우 빠르지만, 물체가 크게 변형되면 광선 추적 효율이 떨어짐.
*   **전략**: 
    *   **Rigid Body (단순 이동)**: Refit 사용.
    *   **Skeletal Animation (캐릭터)**: 매 프레임 Rebuild가 필요할 수도 있음 (정점 위치가 계속 변하기 때문).

### 16.2 Ogre-Next `SceneManager` 동기화
*   오거의 모든 `MovableObject`는 `_updateRenderQueue` 시점에 월드 행렬이 갱신됨.
*   우리는 이 시점에 동적 객체들의 **BLAS(Bottom Level AS)**와 **TLAS(Top Level AS)**를 각각 업데이트하도록 `VulkanVaoManager`를 호출해야 함.

---

## 17. 실전 디노이징 알고리즘 (Denoising in Depth)

실시간 레이트레이싱은 픽셀당 1개 미만의 광선(Low SPP)을 쏘므로 엄청난 노이즈가 발생함. 이를 제거하는 것은 **"선택이 아닌 필수"**임.

### 17.1 SVGF (Spatiotemporal Variance Guided Filtering)
가장 보편적인 하이브리드 디노이저의 구조:
1.  **Temporal Accumulation**: 이전 프레임의 결과물을 **Motion Vector**로 추적해 현재 프레임에 누적함 (안정성 확보).
2.  **Variance Estimation**: 픽셀 간의 밝기 차이(분산)를 계산해 노이즈 정도를 파악.
3.  **A-Trous Filter**: 분산 값을 기반으로 필터 크기를 조절하며 여러 번 블러링(Blur)을 수행. 이때 **Normal, Depth** 정보를 참고해 경계를 보존(Edge-stopping)함.

### 17.2 ASVGF (Adaptive SVGF)
*   움직임이 많은 영역은 이전 프레임 데이터를 적게 쓰고, 정적인 영역은 많이 쓰는 가변적 방식.
*   오거의 `Compositor` 내에서 별도의 포스트 프로세스 패스로 구현하는 것이 가장 효율적임.

---

## 18. 고급 트러블슈팅 및 Validation (Debugging)

Vulkan은 잘못된 명령을 내려도 에러를 뱉지 않고 묵묵히 죽어버림. 이를 방지하기 위한 도구 활용법.

### 18.1 Vulkan Validation Layers (VVL)
*   **설정**: `VulkanDevice` 생성 시 `VK_LAYER_KHRONOS_validation`을 활성화.
*   **경고 메시지**: 배리어 설정 오류, 메모리 할당 위반, 레이아웃 미일치 등을 실시간으로 콘솔에 띄워줌. **(VVL이 조용할 때까지 코딩하는 것이 원칙)**

### 18.2 GPU-Assisted Validation
*   쉐이더 내부의 배열 인덱스 초과(Out of Bounds) 등 런타임 오류를 잡아냄. 성능 부하가 크지만 디버깅의 끝판왕.

### 18.3 Aftermath / Device Lost 대응
*   GPU가 뻗었을 때(TDR), 어느 명령에서 죽었는지 위치를 찾아주는 NVIDIA 전용 라이브러리. `VulkanDevice`에 통합 가능.

---

## 19. 실무 최적화 기법 (Production-Ready Optimization)

성능이 나오지 않는 하이브리드 렌더러는 의미가 없음. GPU의 한계를 끌어쓰는 팁.

### 19.1 쉐이더 가변 속도 렌더링 (VRS / Ray Binning)
*   **Ray Binning**: 비슷한 광선 방향을 가진 광선들끼리 묶어서 처리하여 GPU 캐시 효율을 높임.
*   **VRS (Variable Rate Shading)**: 화면 중앙(중요한 곳)은 1spp, 주변부(덜 중요한 곳)는 0.25spp로 쏘는 기법.

### 19.2 Descriptor Set의 효율적 관리
*   매번 `UpdateDescriptorSet`을 호출하지 말 것. 오거-넥스트의 `Dirty Checking` 메커니즘을 최대한 활용해 **정말 변경된 것만 갱신**하는 로직을 유지해야 함.

---

## 20. 미래 확장성: 패스 트레이싱으로의 진화 (Path Tracing)

우리가 만든 하이브리드 시스템은 결국 **실시간 패스 트레이싱**으로 가는 징검다리임.

### 20.1 재귀적 광선 추적 (Recursive Rays)
*   현재는 하이브리드(1차 반사/그림자만 RT)이지만, 레이트레이싱 엔코더 내에서 `TraceRay`를 재귀적으로 호출하여 **무한 반사**와 **굴절**을 구현할 수 있음.
*   **SBT의 고도화**: 재귀 호출 시 각 재질에 맞는 Closest Hit 쉐이더가 정확히 선택되도록 SBT 구조를 더 정교하게 설계해야 함.

### 20.2 하이브리드의 미래: AI 기반 렌더링
*   **DLSS 3 / FSR 3**: 저해상도 RT 결과를 AI로 고해상도화하는 기술. 오거-넥스트 플러그인 형태로 이들을 통합하는 것이 PRISM의 최종 진화형 중 하나가 될 것임.

---

## 21. 커맨드 버퍼 수명 주기 심층 분석 (Command Buffer Lifecycle)

Vulkan에서 CPU가 명령을 기록하고 GPU가 이를 실행하는 과정은 비동기적임. 오거-넥스트가 이를 어떻게 관리하는지 알아야 레이트레이싱 명령을 안전하게 끼워 넣을 수 있음.

### 21.1 `VulkanDevice::commitAndNextCommandBuffer()`
이 함수는 오거의 심장 박동임.
*   **Fence 대기**: 이전 프레임의 작업이 끝났는지 확인 (`vkWaitForFences`).
*   **Command Pool 리셋**: 다 쓴 버퍼를 초기화하고 재사용.
*   **Submission**: 기록된 버퍼를 `vkQueueSubmit`으로 GPU에 전송.
*   **PRISM 팁**: 레이트레이싱 패스가 너무 길어지면 여기서 'Stall(지연)'이 발생함. 이를 방지하려면 여러 개의 커맨드 버퍼를 병렬로 기록하거나 **비동기 컴퓨트**를 사용해야 함.

### 21.2 링 버퍼(Ring Buffer) 방식의 상수 관리
*   오거는 매 프레임 변하는 데이터(행렬 등)를 관리하기 위해 거대한 `Buffer`를 링 형태로 사용함. 
*   레이트레이싱용 **SBT(Shader Binding Table)**나 **가속 구조 업데이트 데이터**도 이 링 버퍼 구조를 활용해 메모리 단편화를 방지해야 함.

---

## 22. 비동기 컴퓨트 활용 (Async Compute for Denoising)

그래픽 큐(Graphics Queue)가 래스터화를 수행하는 동안, **비동기 컴퓨트 큐(Async Compute Queue)**에서 레이트레이싱이나 디노이징을 동시에 수행하는 기법임.

### 22.1 오거-넥스트에서의 구현 지점
*   **`VulkanQueue` 확장**: 기존 그래픽 큐 외에 `mComputeQueue`를 별도로 확보.
*   **세마포어(Semaphore) 동기화**: 그래픽 큐가 G-Buffer 생성을 마치면 세마포어를 시그널하고, 컴퓨트 큐가 이를 받아 디노이징을 시작함.
*   **성능 이점**: GPU의 연산 유닛(ALU)을 쉬지 않게 하여 전체 프레임 타임을 20~30% 단축 가능.

---

## 23. Vulkan 페이징 및 리소스 상주 전략 (Resource Residency)

하이브리드 렌더러는 텍스처, 정점 데이터, 가속 구조 등 방대한 메모리를 소모함. VRAM이 부족할 때를 대비한 전략.

### 23.1 `VaoManager`의 정적/동적 버퍼 분리
*   **Static**: 배경 등 변하지 않는 데이터 → `Device Local` 메모리에 고정.
*   **Dynamic**: 캐릭터 등 매 프레임 변하는 데이터 → `Host Visible` 또는 `Unified Memory` 활용.
*   **PRISM 전략**: 레이트레이싱용 **BLAS**는 크기가 매우 크므로, 화면에 보이지 않는 물체의 BLAS는 메모리에서 해제하거나 낮은 정밀도로 관리하는 **"가속 구조 페이징"** 도입 검토 필요.

---

## 24. 커스텀 컴포지터 패스 코드 구현 (Custom Pass Code-Level)

실제로 오거 엔진에 `PASS_RAYTRACING`을 추가하기 위한 코드 수정 가이드임.

### 24.1 `OgreCompositorPassDef.h` 수정
*   `enum CompositorPassType`에 `PASS_RAYTRACING` 추가.
*   해당 패스가 필요로 하는 데이터(사용할 RT 쉐이더 이름, 입력 G-Buffer 등)를 담을 `CompositorPassRayTracingDef` 클래스 정의.

### 24.2 `OgreCompositorPassRayTracing.cpp` 생성
*   `CompositorPass`를 상속받아 `execute()` 함수 구현.
*   이 `execute()` 내부에서 `VulkanRenderSystem`의 레이트레이싱 전용 함수를 호출하도록 연결.

---

## 25. SBT (Shader Binding Table) 메모리 레이아웃 및 정렬

SBT는 단순한 배열이 아님. Vulkan 사양에 따라 엄격한 **메모리 정렬(Alignment)** 규칙을 지켜야 함.

### 25.1 메모리 구조 (SBT Layout)
*   **Ray Generation Section**: 단일 엔트리. `vkGetRayTracingShaderGroupHandlesKHR`로 가져온 핸들을 저장.
*   **Miss Section**: 배경 처리용 쉐이더 핸들.
*   **Hit Group Section**: (가장 복잡함) 각 물체(Instance)별로 `Closest Hit`, `Any Hit` 쉐이더 핸들과 그들이 사용할 **인라인 상수 데이터**를 포함 가능.

### 25.2 정렬 규칙 (Alignment Rules)
*   **Base Alignment**: `VkPhysicalDeviceRayTracingPipelinePropertiesKHR::shaderGroupBaseAlignment`. (보통 64바이트)
*   **Handle Alignment**: `shaderGroupHandleAlignment`. (보통 32바이트)
*   **PRISM 팁**: 오거의 `VaoManager`를 통해 SBT용 전용 버퍼를 할당할 때, 이 정렬 값들을 고려해 패딩(Padding)을 정확히 넣어야 함. 그렇지 않으면 GPU가 엉뚱한 메모리를 읽어 TDR(Crash) 발생.

---

## 26. HLMS 재질 데이터 추출 및 RT 연동 (Material Bridge)

레이트레이싱 쉐이더는 기존 HLMS 재질 데이터(Diffuse, Roughness 등)에 어떻게 접근하는가?

### 26.1 `HlmsDatablock` 데이터 노출
*   **방법**: 모든 `HlmsPbsDatablock`의 데이터를 하나의 거대한 **Structured Buffer**에 모아서 저장함.
*   **인덱싱**: SBT의 각 Hit Group 엔트리에 해당 물체가 사용하는 `Datablock`의 인덱스를 저장.
*   **쉐이더 코드**:
    ```hlsl
    // Ray Hit Shader 내에서
    MaterialData mat = AllMaterials[sbtData.materialIndex];
    float3 color = CalculateBRDF(mat, normal, rayDir);
    ```

### 26.2 글로벌 텍스처 리소스 (Bindless Textures)
*   레이트레이싱은 씬 전체를 보므로, 모든 텍스처가 한꺼번에 바인딩된 **Bindless Texture** 시스템이 필수적임.
*   오거-넥스트의 `Descriptor Set` 관리를 확장하여 레이트레이싱 패스 시 모든 활성 텍스처를 하나의 거대한 텍스처 배열로 전달해야 함.

---

## 27. 실전 프로파일링 워크플로우 (Profiling Workflow)

하이브리드 렌더러의 성능 병목을 잡는 실무 기술임.

### 27.1 RenderDoc 프레임 캡처 분석
*   **체크리스트**:
    1.  **Event Browser**: G-Buffer 패스와 RT 패스 사이의 배리어가 정확한가?
    2.  **Pipeline State**: RT 파이프라인의 SBT가 의도한 대로 구성되었는가?
    3.  **Mesh Viewer**: 가속 구조(AS) 빌드를 위해 전달된 정점 데이터가 올바른가?

### 27.2 NVIDIA Nsight Graphics (Performance)
*   **GPU Trace**: 그래픽 큐와 컴퓨트 큐의 병렬 실행(Async Compute)이 실제로 일어나고 있는지 타임라인 확인.
*   **Ray Tracing Statistics**: 
    *   **Invocations**: 광선이 너무 많이 쏘이고 있지는 않은가?
    *   **AS Traversal**: 가속 구조 탐색에 걸리는 시간이 너무 길지는 않은가? (가속 구조 재빌드 필요성 판단)

---

## 28. 글로벌 일루미네이션 전략 (Global Illumination: GI)

단순한 직접 조명(Direct Light)을 넘어, 주변 물체로부터 반사된 빛(Indirect Light)을 실시간으로 계산하는 기술임.

### 28.1 DDGI (Dynamic Diffuse Global Illumination)
*   **원리**: 씬 전체에 **광선 프로브(Probes)**를 그리드 형태로 배치하고, 각 프로브가 레이트레이싱으로 주변 밝기를 주기적으로 업데이트함.
*   **장점**: 빛샘(Light Leaks) 현상이 적고 동적인 환경 변화에 강함.
*   **PRISM 전략**: 오거의 `SceneManager`를 통해 씬의 경계(Bounding Box)를 파악하고, 자동으로 프로브 그리드를 생성하는 시스템 구축.

### 28.2 ReSTIR (Reservoir Spatio-Temporal Importance Resampling)
*   **원리**: 수많은 광원 중 가장 중요한 광원(중요 샘플)만을 골라내는 통계적 기법.
*   **장점**: 수천 개의 광원을 실시간으로 처리 가능하며, 노이즈가 적음.
*   **학습 키워드**: `Resampling`, `Reservoirs`, `Temporal/Spatial Reuse`.

---

## 29. 투명도 처리 기법 (Handling Transparency)

레이트레이싱에서 유리, 물, 나뭇잎(Alpha Testing)은 매우 까다로운 주제임.

### 29.1 Any-Hit Shader의 활용
*   **Alpha Testing (나뭇잎 등)**: 광선이 물체에 부딪혔을 때, 해당 픽셀의 알파 값을 체크하여 투명하면 부딪히지 않은 것으로 간주하고 통과시킴 (`ignoreIntersection`).
*   **성능 주의**: Any-Hit 쉐이더는 호출 빈도가 매우 높으므로 극도로 가볍게 작성해야 함.

### 29.2 굴절과 반사 (Glass/Water)
*   **재귀적 광선**: Closest Hit 쉐이더에서 다시 `TraceRay`를 호출해 굴절 광선을 쏨.
*   **프레넬(Fresnel) 효과**: 입사각에 따라 반사와 굴절의 비율을 결정하는 물리 법칙 적용 필수.

---

## 30. PRISM 비전: 통합 하이브리드 파이프라인 (Unified Vision)

우리가 지금까지 배운 모든 기술을 하나의 유기적인 엔진으로 완성하는 최종 설계도임.

### 30.1 파이프라인의 완성 (The Unified Flow)
1.  **Scene Analysis**: 동적/정적 객체 구분 및 가속 구조(AS) 업데이트.
2.  **Rasterization (G-Buffer)**: 고해상도 지오메트리 정보 및 모션 벡터 추출.
3.  **Ray Tracing**: GI, 그림자, 반사광을 비동기 컴퓨트(Async Compute)로 계산.
4.  **Denoising (ASVGF)**: AI 또는 필터링 기법으로 고품질 이미지 정제.
5.  **Composition**: 래스터 결과와 RT 결과를 물리 기반으로 합성.
6.  **Post-Processing**: DLSS/FSR 업스케일링 및 톤 매핑(Tone Mapping).

### 30.2 PRISM의 목표
*   **Visual Fidelity**: 영화 같은 실시간 그래픽 구현.
*   **Performance**: 최신 GPU 기능을 최대한 활용한 60 FPS 달성.
*   **Extensibility**: 누구나 쉽게 레이트레이싱 기능을 추가할 수 있는 오거-넥스트 기반 프레임워크 제공.

---

## 31. 중요도 샘플링 및 몬테카를로 (Importance Sampling & Monte Carlo)

레이트레이싱의 품질은 **"어떤 방향으로 광선을 쏘는가"**에 달려 있음.

### 31.1 몬테카를로 적분 (Monte Carlo Integration)
*   **원리**: 복잡한 물리 함수(렌더링 방정식)를 풀기 위해 수많은 무작위 샘플을 평균내어 근사값을 구하는 방식.
*   **문제점**: 샘플 수가 적으면 이미지에 반점(Noise)이 생김.

### 31.2 중요도 샘플링 (Importance Sampling)
*   **원리**: 무작위가 아닌, 빛이 많이 반사될 것 같은 방향(예: 거울 반사 방향)으로 더 많은 광선을 쏘는 기법.
*   **결과**: 동일한 샘플 수로도 훨씬 깨끗하고 노이즈 없는 이미지를 얻을 수 있음.
*   **학습 키워드**: `PDF (Probability Density Function)`, `Cosine Weighted Sampling`.

---

## 32. BRDF 및 PBR의 레이트레이싱 통합 (BRDF & PBR)

물체의 질감을 물리적으로 정확하게 표현하기 위한 수학적 모델임.

### 32.1 Cook-Torrance BRDF
현대 PBR의 표준 모델:
*   **D (Distribution)**: 미세면(Microfacet)의 분포. (GGX 모델 사용)
*   **F (Fresnel)**: 입사각에 따른 반사율 변화. (Schlick 근사 사용)
*   **G (Geometry)**: 미세면 간의 그림자(Shadowing) 및 차폐(Masking).

### 32.2 쉐이더 구현 (Shader Integration)
*   오거-넥스트의 `HlmsPbs`에서 사용하는 BRDF 로직을 레이트레이싱용 `Closest Hit Shader`에 그대로 이식하여 래스터화와 레이트레이싱 결과가 일치하도록 만듦.

---

## 33. 하드웨어 벤더별 최적화 전략 (Vendor Optimizations)

NVIDIA와 AMD는 레이트레이싱을 처리하는 하드웨어 구조가 다름.

### 33.1 NVIDIA (RT Cores)
*   **특징**: 가속 구조(AS) 탐색 및 삼각형 충돌 검사를 전용 하드웨어(RT Core)가 수행.
*   **최적화**: `RTX 전용 확장 기능을 적극 활용하고, Inline Ray Tracing보다는 Ray Tracing Pipeline 방식이 유리할 수 있음.

### 33.2 AMD (Ray Accelerators)
*   **특징**: 텍스처 유닛 일부를 레이트레이싱 연산에 활용.
*   **최적화**: 컴퓨트 큐를 활용한 `Inline Ray Tracing` 성능이 상대적으로 우수할 수 있으며, 가속 구조를 가볍게 유지하는 것이 중요함.

---

## 34. 수천 개의 광원 관리 (Managing Many Lights: Clustered)

레이트레이싱은 씬에 있는 모든 광원을 고려해야 하므로, 광원이 많아질수록 기하급수적으로 느려짐.

### 34.1 라이트 클러스터링 (Light Clustered)
*   **원리**: 화면을 3D 격자(Cluster)로 나누고, 각 격자에 영향을 주는 광원 목록을 미리 계산해 둠.
*   **RT 통합**: 광선이 부딪힌 지점의 위치를 기반으로 어떤 클러스터에 속하는지 파악하고, 해당 클러스터의 광원들만 샘플링하여 연산량을 획기적으로 줄임.
*   **오거 연동**: 오거의 `Forward+` 또는 `Clustered Forward` 데이터 구조를 레이트레이싱 패스에서도 공유하여 사용.

---

## 35. 재귀적 광선 추적의 최적화 (Recursive RT Optimization)

재귀 호출(Recursive Call)은 GPU 스택 메모리를 소모하며 성능을 저하시킴.

### 35.1 루프 기반 광선 추적 (Iterative Ray Tracing)
*   **전략**: 재귀 호출 대신 `while` 루프와 **Ray Payload**를 활용해 다음 광선 정보를 저장하고 반복적으로 추적함.
*   **이점**: 스택 오버플로우 방지 및 레지스터 사용량 최적화로 성능 향상.

### 35.2 하이브리드 반사 (Hybrid Reflections)
*   **전략**: 거친 표면(Rough Surface)은 저렴한 래스터화 반사(SSR)를 쓰고, 매끄러운 표면(Glossy Surface)만 레이트레이싱으로 처리하는 혼합 방식 적용.

---

## 36. 실전 디버깅 체크리스트 (Debugging Checklist)

레이트레이싱 구현 중 문제가 생겼을 때 확인해야 할 핵심 항목들임.

1.  **가속 구조(AS)**: 물체가 비정상적으로 보이거나 투명해 보이면 BLAS/TLAS 빌드 시의 **Index/Vertex Buffer 오프셋**이 정확한지 확인.
2.  **SBT 정렬**: GPU Crash가 발생하면 SBT 엔트리의 **64바이트 정렬(Alignment)**이 지켜졌는지 재확인.
3.  **배리어(Barrier)**: 화면이 깜빡거리거나 이전 프레임 잔상이 남으면 **Image Layout Transition** 시점이 올바른지 확인.
4.  **쉐이더 핸들**: 엉뚱한 쉐이더가 실행되면 `vkGetRayTracingShaderGroupHandlesKHR`로 가져온 핸들이 SBT에 순서대로 기록되었는지 확인.

---

## 37. 파티클 시스템의 레이트레이싱 (RT for Particles)

불꽃, 연기 등 수만 개의 파티클을 레이트레이싱으로 그리는 것은 성능상 매우 비쌈.

### 37.1 빌보드(Billboard) 레이트레이싱
*   **전략**: 각 파티클을 단순한 사각형(Quad)으로 간주하고, `Any-Hit Shader`에서 파티클 텍스처의 알파 값에 따라 광선을 투과시킴.
*   **최적화**: 멀리 있는 파티클은 레이트레이싱 대신 래스터화로 그리고, RT 결과와 섞어주는 하이브리드 방식 권장.

---

## 38. 물리 기반 카메라 모델 (Physical Camera Model)

현실적인 이미지를 위해 카메라의 물리적 특성을 시뮬레이션해야 함.

### 38.1 피사체 외 분산 (Depth of Field: DoF)
*   **원리**: 레이트레이싱에서 `Ray Generation Shader`가 단일 점이 아닌 렌즈(Aperture) 영역 내의 무작위 지점에서 광선을 쏘도록 설정.
*   **결과**: 초점이 맞지 않는 부분에 자연스러운 '보케(Bokeh)' 효과가 생김.

### 38.2 노출(Exposure) 및 톤 매핑(Tone Mapping)
*   **HDR 통합**: 레이트레이싱은 매우 넓은 밝기 범위를 생성하므로, 이를 사람이 볼 수 있는 범위로 변환하는 `ACES` 또는 `Reinhard` 톤 매핑 적용 필수.

---

## 39. UI 및 HUD 합성 전략 (UI & HUD Integration)

레이트레이싱 패스는 보통 선형 색 공간(Linear Space)에서 일어나지만, UI는 sRGB 공간에서 작업됨.

### 39.1 최종 합성 순서 (Composition Order)
1.  **RT + Raster Lighting**: 3D 씬의 모든 라이팅 및 포스트 프로세스 완료.
2.  **Gamma Correction**: sRGB 공간으로 변환.
3.  **UI Overlay**: 최종 화면 위에 UI를 그림.
*   **주의**: UI가 3D 씬의 빛에 영향을 받거나(예: 홀로그램 UI), UI가 씬을 가리는 처리를 할 때 **Depth Buffer**를 공유해야 함.

---

## 40. 멀티 스레드 AS 빌드 전략 (Multi-threaded AS Build)

가속 구조(AS) 빌드는 CPU 부하가 크므로, 메인 스레드에서 처리하면 프레임 드랍이 발생함.

### 40.1 병렬 빌드 시스템
*   **전략**: 오거-넥스트의 `TaskScheduler`를 활용해 각 물체(BLAS)의 빌드 명령을 여러 스레드에 분산.
*   **Vulkan 통합**: 각 스레드에서 별도의 `Command Pool`을 사용해 커맨드 버퍼를 기록하고, 메인 스레드에서 하나로 모아 제출(`vkQueueSubmit`).

---

## 41. 고급 레이트레이싱 확장 (Advanced RT: RTXDI & RTXGI)

NVIDIA의 최신 레이트레이싱 기술을 엔진에 통합하는 방법.

### 41.1 RTXDI (Direct Illumination)
*   수백만 개의 동적 광원을 샘플링 노이즈 없이 실시간으로 처리하는 기술.
*   **PRISM 팁**: G-Buffer와 가속 구조 정보를 RTXDI 라이브러리에 전달하여 고품질 직접 조명 구현.

### 41.2 RTXGI (Global Illumination)
*   레이트레이싱 프로브를 자동으로 배치하고 라이팅을 업데이트하는 기술.
*   **PRISM 팁**: 앞서 배운 DDGI를 NVIDIA 기술로 대체하여 안정적인 전역 조명 확보.

---

## 42. PRISM 프로젝트 6개월 개발 로드맵 (Roadmap)

우리가 지금까지 설계한 내용을 실제 제품으로 완성하기 위한 단계적 계획임.

### [1-2개월] 하이브리드 엔진 코어 완성
*   Vulkan RT 확장 활성화 및 `VulkanQueue` 엔코더 확장.
*   G-Buffer 및 기본적인 레이트레이싱 그림자/반사 구현.

### [3-4개월] 시각적 품질 고도화
*   SVGF 디노이징 및 중요도 샘플링(Importance Sampling) 적용.
*   SBT와 HLMS 데이터 연동을 통한 PBR 재질 완성.

### [5-6개월] 최적화 및 확장 기능 통합
*   Async Compute 적용 및 멀티 스레드 AS 빌드 구현.
*   RTXDI/RTXGI 통합 및 패스 트레이싱 모드(Offline Quality) 추가.

---

## 43. 고성능 레이트레이싱 쉐이더 작성 비법 (Shader Optimization)

쉐이더 코드는 GPU에서 수백만 번 실행되므로 한 줄의 최적화가 전체 성능을 좌우함.

### 43.1 레지스터 사용량 최소화 (Minimize Registers)
*   **원리**: 쉐이더 내부의 지역 변수가 많아지면 레지스터가 부족해져 병렬 처리 능력(Occupancy)이 떨어짐.
*   **팁**: 복잡한 계산은 중간 결과를 변수에 담지 말고 직접 연산하거나, `min16float` 등 저정밀도 타입을 적절히 활용.

### 43.2 분기문 최적화 (Divergence Control)
*   **원리**: 동일한 워프(Warp) 내의 스레드들이 서로 다른 분기문을 타면 성능이 급격히 저하됨.
*   **팁**: 레이트레이싱에서는 비슷한 방향의 광선을 쏘는 픽셀들을 묶어서(Ray Binning) 분기를 일치시키는 것이 효과적임.

---

## 44. 하드웨어 제약 사항 및 대체 전략 (Hardware Fallbacks)

모든 사용자가 최신 RTX 카드를 가진 것은 아님. 하드웨어 스펙에 따른 유연한 대응이 필요함.

### 44.1 하드웨어 가속 미지원 시 대체 (Software Ray Tracing)
*   **전략**: 레이트레이싱 하드웨어가 없는 경우, 컴퓨트 쉐이더를 활용한 소프트웨어 기반 레이트레이싱(예: `SDF Shadow`)으로 전환.
*   **오거 통합**: 엔진 초기화 시 하드웨어 기능을 체크하고, 자동으로 적절한 컴포지터 패스를 선택하게 만듦.

---

## 45. PRISM 프로젝트의 최종 비전 (Final Vision)

우리는 단순히 화면을 예쁘게 만드는 것을 넘어, **"엔진의 한계를 뛰어넘는 기술적 정수"**를 추구함.

### 45.1 기술적 민주화 (Democratizing High-end Tech)
*   오거-넥스트라는 오픈 소스 엔진 위에서 최첨단 하이브리드 레이트레이싱 기술을 누구나 사용할 수 있도록 프레임워크를 공개하여, 중소형 개발팀도 대형 엔진급의 그래픽을 구현할 수 있게 돕는 것.

### 45.2 끊임없는 진화 (Continuous Evolution)
*   PRISM은 고정된 결과물이 아님. 레이트레이싱 기술이 발전함에 따라 새로운 논문과 기법을 지속적으로 흡수하여, 전 세계 그래픽 개발자들의 영감이 되는 프로젝트로 성장할 것임.

---

## 46. 실전 협업 전략 (Collaboration: Git Workflow)

엔진 개발은 혼자 하는 것이 아님. 코드의 안정성을 지키기 위한 협업 규칙.

### 46.1 기능 기반 브랜치 전략 (Feature Branching)
*   **전략**: `main` 브랜치는 항상 빌드 가능한 상태 유지. 새로운 기능(예: `feature/denoising`)은 별도 브랜치에서 작업 후 PR(Pull Request)을 통해 머지.
*   **코드 리뷰**: 레이트레이싱 같은 복잡한 코드는 반드시 동료의 리뷰를 거쳐 성능 저하나 동기화 에러(Barrier missing)를 사전에 차단.

---

## 47. 테스트 및 QA 자동화 (Test & QA Automation)

그래픽 엔진은 눈으로 확인하는 것만으로는 부족함. 자동화된 검증이 필수적임.

### 47.1 스크린샷 비교 테스트 (Snapshot Testing)
*   **방법**: 특정 씬을 렌더링하고, 이를 기준 이미지(Golden Image)와 픽셀 단위로 비교하여 렌더링 결과에 오류가 생겼는지 자동 감지.
*   **CI/CD 통합**: 코드를 올릴 때마다 GitHub Actions 등을 통해 자동으로 빌드하고 테스트 씬을 실행하여 무결성 확인.

---

## 48. 사용자 커뮤니티 구축 방안 (Building Community)

프로젝트가 살아남으려면 사용하는 사람이 있어야 함.

### 48.1 문서화 및 튜토리얼 (Documentation)
*   우리가 지금 작성하고 있는 이 가이드처럼, 새로운 개발자가 쉽게 PRISM에 기여할 수 있도록 상세한 위키(Wiki)와 샘플 코드를 제공해야 함.
*   **Showcase**: PRISM으로 만든 멋진 데모 영상을 통해 커뮤니티의 관심을 유도.

---

## 49. 당신은 이제 엔진 개발자입니다

지금까지 방대한 기술 로드맵을 함께 훑어왔습니다. 

레이트레이싱, 불칸, 오거-넥스트, HLMS... 이 모든 어려운 개념들은 이제 당신의 머릿속에 하나의 지도로 그려져 있을 것입니다. 처음에는 막막했을지 모르지만, 끊임없이 탐구해온 당신의 열정은 이미 **Senior Graphics Engineer**의 그것과 다를 바 없습니다.

엔진 개발은 끝이 없는 여정입니다. 하지만 두려워하지 마세요. 당신에게는 이제 이 설계도가 있고, 무엇을 공부해야 할지 알며, 무엇보다 **"동기화 배리어 하나에 밤을 지새울 수 있는 끈기"**가 생겼으니까요.

---

# 🏆 PRISM 하이브리드 렌더러 가이드: 최종 마침표

본 문서는 당신이 오거-넥스트 엔진 위에 최고의 불칸 하이브리드 렌더러를 구축하기 위한 모든 지식을 담았습니다.

*   **기초**: Vulkan 아키텍처와 오거의 추상화 계층 이해.
*   **핵심**: 가속 구조(AS), SBT, HLMS 데이터 연동.
*   **고급**: 디노이징(SVGF), GI(DDGI), 비동기 컴퓨트.
*   **실무**: 최적화, 협업, 로드맵, 그리고 엔진 개발자의 마음가짐.

이제 코드를 작성할 시간입니다. **첫 번째 Ray가 화면에 그려지는 그 순간의 전율**을 꼭 느껴보시기 바랍니다. 당신의 PRISM 프로젝트가 실시간 그래픽의 새로운 지평을 열기를 진심으로 응원합니다!

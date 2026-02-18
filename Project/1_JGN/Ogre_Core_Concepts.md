# 🗺️ Ogre-Next 핵심 개념 가이드 (PRISM 프로젝트)

이 문서는 Ogre-Next 엔진의 아키텍처를 이해하고 효율적으로 제어하기 위한 핵심 오브젝트 및 개념 정리서임.

---

## 1. 엔진의 기초 (The Foundation)

### 1.1 Root
*   **역할**: 엔진의 엔트리 포인트이자 최상위 객체.
*   **기능**: 모든 매니저(HlmsManager, RenderSystem, ResourceGroupManager 등)를 소유하며 프레임 루프를 구동함.

### 1.2 RenderSystem (Vulkan/GL3Plus/Metal)
*   **역할**: 하드웨어 API(Vulkan 등)와의 직접적인 통로.
*   **기능**: 엔진의 추상화된 렌더링 명령을 실제 API 호출로 변환 및 리소스 생명 주기 관리.

---

## 2. 씬 구성 요소 (Scene Hierarchy)

### 2.1 SceneManager
*   **역할**: 월드(Scene)의 주인. 모든 물체, 조명, 카메라를 관리함.
*   **특징**: 씬 그래프(Scene Graph)를 유지하며 가시성 판단(Culling)을 수행하고 렌더 큐(Render Queue)를 생성함.

### 2.2 SceneNode
*   **역할**: 위치(Position), 회전(Rotation), 스케일(Scale) 정보를 담는 노드.
*   **특징**: 계층 구조(Parent-Child)를 가지며, 실제 물체(Item)는 이 노드에 '부착(Attach)'되어야 화면에 나타남.

### 2.3 MovableObject
*   **역할**: 씬에 배치될 수 있는 모든 동적 객체의 부모 클래스.
*   **하위 클래스**: Item, Light, Camera, ParticleSystem 등.

### 2.4 변환 계층 구조 (Transformation Hierarchy)
*   **상대 좌표 (Local Space)**: 자식 노드는 항상 부모 노드를 기준으로 한 상대적인 위치/회전/스케일 값을 가짐.
*   **상속 (Inheritance)**: 부모 노드(`RobotNode`)를 이동/회전시키면 부착된 모든 객체와 자식 노드(`SwordNode`)가 함께 변함.
*   **절대 좌표 (World Space)**: 엔진은 렌더링 시점에 부모와 자식의 행렬을 계산하여 최종적인 월드 좌표를 산출함.

---

## 3. 시각적 데이터 (Visual Elements)

### 3.1 Item (Ogre-Next의 핵심)
*   **역할**: 기존 Ogre 1.x의 'Entity'를 대체하는 현대적인 물체 단위.
*   **특징**: 멀티 스레딩과 캐싱에 최적화되어 있으며, 메시 데이터와 재질(HlmsDatablock)을 연결하는 실제 렌더링 객체임.

### 3.2 Mesh & SubMesh
*   **Mesh**: 기하학적 데이터(정점, 인덱스)를 담은 리소스 파일.
*   **SubMesh**: 메시의 하위 단위. 하나의 메시는 여러 개의 SubMesh로 구성될 수 있으며, 각 SubMesh는 서로 다른 재질을 가질 수 있음.

### 3.3 VaoManager (Vertex Array Object Manager)
*   **역할**: GPU 메모리(Buffer) 및 정점 선언을 관리하는 로우레벨 매니저.
*   **기능**: Vulkan의 정점/인덱스/상수 버퍼를 생성하고 GPU로 데이터를 전송함.

---

## 4. 렌더링 파이프라인 (The Pipeline)

### 4.1 Compositor (스크립트 기반 파이프라인)
*   **역할**: 프레임이 그려지는 "순서"와 "방법"을 정의하는 엔진의 핵심 워크플로우.
*   **구성**:
    *   **Workspace**: 렌더링 결과가 출력되는 최종 도화지. (보통 RenderWindow와 연결)
    *   **Node**: 개별 렌더링 단계(예: Shadow Pass Node, Main Render Node, Post-processing Node).
    *   **Pass**: 노드 내부의 구체적인 작업(Clear, Render Scene, Quad, Compute 등).

---

## 5. 리소스 및 재질 (Resources & Materials)

### 5.1 ResourceGroupManager
*   **역할**: 메시, 텍스처, 쉐이더 파일의 물리적 경로를 관리하고 필요할 때 로딩/언로딩을 수행.

### 5.2 Hlms (High Level Material System)
*   **역할**: 씬의 환경(조명, 안개 등)과 재질 설정을 분석해 실시간으로 최적화된 쉐이더를 조립하는 시스템. (별도 문서 `HLMS_Deep_Dive.md` 참조)

---

## 💡 오거-넥스트의 멘탈 모델 (Mental Model)

1.  **Tree (SceneGraph)**: `SceneNode`를 통해 물체의 계층과 위치를 잡음.
2.  **Pipe (Compositor)**: `Workspace`를 통해 렌더링의 흐름(Flow)을 설계함.
3.  **Factory (HLMS)**: `Datablock` 설정을 기반으로 쉐이더 소스 코드를 찍어냄.
---

## 6. 실전 예시 (Real-world Examples)

오거-넥스트의 핵심 개념들이 어떻게 유기적으로 연결되는지 구체적인 시나리오를 통해 설명함.

### 6.1 시나리오: "빛나는 빨간색 구(Sphere)를 씬에 배치하기"

1.  **데이터 준비 (Mesh)**: `Sphere.mesh` 파일을 메모리에 로드함. (VaoManager가 정점 버퍼를 GPU에 올림)
2.  **재질 설정 (Datablock)**: `HlmsPbsDatablock`을 생성하고 컬러를 'Red', 속성을 'Emissive(발광)'로 설정함.
3.  **물체 생성 (Item)**: `Sphere.mesh` 데이터와 방금 만든 'Red' `HlmsDatablock`을 연결하는 `Item` 객체를 생성함.
4.  **위치 결정 (SceneNode)**: 씬의 루트(Root) 노드 아래에 자식 노드를 만들고 좌표를 `(10, 0, 0)`으로 설정한 뒤 `Item`을 부착(Attach)함.
5.  **렌더링 (Compositor)**: `Workspace`가 생성되면서 'MainRenderNode' 내의 `RenderScenePass`가 실행됨. `VulkanRenderSystem`을 통해 Draw Call이 발생함.

### 6.2 씬 그래프(SceneGraph) 시각화 예시

```text
Root SceneNode (0, 0, 0)
 └── RobotNode (10, 0, 0)
      ├── RobotItem (Mesh: robot.mesh, Datablock: MetalPbs)
      └── SwordNode (0, 5, 0)  <-- 로봇 노드 기준 상대 좌표
           └── SwordItem (Mesh: sword.mesh, Datablock: SteelPbs)
```
*   **특징**: 부모인 `RobotNode`가 움직이면 자식인 `SwordNode`와 `SwordItem`도 함께 월드를 이동함.

---

## 7. 객체 생성 가이드 (Creation Guide)

오거-넥스트의 모든 객체는 각자의 상위 매니저를 통해 생성되는 **팩토리 패턴**을 따름.

### 7.1 SceneManager 생성
*   **API**: `Root::createSceneManager()`
```cpp
Ogre::SceneManager *sceneManager = root->createSceneManager(
    Ogre::ST_GENERIC,           // 씬 타입
    numThreads,                 // 멀티스레드 개수
    "MySceneManager"            // 매니저 이름
);
```

### 7.2 Item 생성 (외형 데이터)
*   **API**: `SceneManager::createItem()`
```cpp
Ogre::Item *item = sceneManager->createItem(
    "Sphere.mesh",              // 메시 파일 이름
    "General",                  // 리소스 그룹 이름
    Ogre::SCENE_DYNAMIC         // 메모리 타입 (DYNAMIC/STATIC)
);
```

### 7.3 SceneNode 생성 및 부착 (위치 정보)
*   **API**: `SceneNode::createChildSceneNode()`, `attachObject()`
```cpp
// 루트 노드 아래에 자식 노드 생성
Ogre::SceneNode *node = sceneManager->getRootSceneNode()->createChildSceneNode(
    Ogre::SCENE_DYNAMIC         // 메모리 타입
);
node->setPosition( 10, 0, 0 );  // 위치 설정
node->attachObject( item );     // Item 부착 (이제 화면에 나타남)
```

### 7.4 HlmsDatablock 생성 (재질 설정)
*   **API**: `Hlms::createDatablock()`
```cpp
Ogre::HlmsPbs *hlmsPbs = static_cast<Ogre::HlmsPbs*>(
    hlmsManager->getHlms(Ogre::HLMS_PBS)
);

Ogre::HlmsPbsDatablock *datablock = static_cast<Ogre::HlmsPbsDatablock*>(
    hlmsPbs->createDatablock(
        "MyRedMaterial",        // 재질 이름
        "MyRedMaterial",        // (내부용)
        Ogre::HlmsMacroblock(), // 래스터라이저 설정
        Ogre::HlmsBlendblock(), // 블렌딩 설정
        Ogre::HlmsParamVec()    // 파라미터
    )
);
datablock->setDiffuse( Ogre::Vector3(1, 0, 0) ); // 빨간색 설정
item->setDatablock( datablock );                // Item에 재질 적용
```

---

## 8. 메모리 타입 선택 (SCENE_STATIC vs SCENE_DYNAMIC)

오거-넥스트는 성능 최적화를 위해 객체 생성 시 메모리 위치를 지정해야 함.

*   **SCENE_STATIC**: 건물, 나무 등 위치가 거의 변하지 않는 물체. (업데이트 비용 낮음)
*   **SCENE_DYNAMIC**: 캐릭터, 총알 등 매 프레임 위치가 변하는 물체. (업데이트 최적화)
*   **주의**: 한 번 설정하면 바꿀 수 없으므로, 생성 시 신중히 결정해야 함.

---

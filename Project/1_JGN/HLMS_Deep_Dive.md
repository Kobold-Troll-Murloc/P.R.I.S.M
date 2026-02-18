# 🏭 Ogre-Next HLMS (High Level Material System) 심층 분석

이 문서는 Ogre-Next의 핵심인 HLMS의 작동 원리를 파악하고, PRISM 프로젝트의 레이트레이싱 파이프라인과 연동하기 위한 종합 가이드임.

---

## 1. HLMS란 무엇인가? (The "Why")

과거의 렌더링 방식에서는 개발자가 `MyMaterial.glsl` 파일을 직접 썼습니다. 하지만 현대 렌더링에서는 다음과 같은 수많은 조합이 발생합니다:
- 텍스처가 1개인 경우 vs 5개인 경우
- 조명이 Point Light인 경우 vs Spot Light인 경우
- 안개가 낀 경우 vs 안 낀 경우
- 그림자가 있는 경우 vs 없는 경우

이 모든 조합을 수동으로 작성하면 수만 개의 파일이 필요합니다. **HLMS는 C++에서 설정한 속성에 따라 필요한 쉐이더 코드를 즉석에서 '조립'해주는 공장**입니다.

---

## 2. HLMS vs 유니티 Material (개념 비교)

유니티의 **Material (.mat)**과 오거의 **HlmsDatablock**은 사용 목적은 같으나 내부 작동 철학이 다름.

| 비교 항목 | 유니티 (Unity) Material | 오거-넥스트 (Ogre-Next) HLMS |
| :--- | :--- | :--- |
| **핵심 개념** | **완제품 (Ready-made)** | **조립 라인 (Assembly Line)** |
| **작동 방식** | 미리 작성된 Uber Shader 내의 기능을 켬. | 필요한 코드 조각(Piece)만 골라 소스를 생성. |
| **최적화** | 안 쓰는 코드도 쉐이더에 포함될 수 있음. | 텍스처가 없으면 샘플링 코드 자체가 생성 안 됨. |
| **데이터 전달** | `SetInt`, `SetFloat` 등 개별 변수 전달. | 수천 개의 재질 정보를 거대한 버퍼로 한 번에 전달. |
| **커스터마이징** | 엔진 내장 쉐이더 수정이 까다로움. | **공장의 부품(.hlms)**을 직접 수정/교체 가능. |

---

## 3. HLMS의 4대 구성 요소

### 3.1 HlmsDatablock (사용자의 주문서)
*   **역할**: 특정 물체에 적용되는 구체적인 재질 값(Diffuse Color, Roughness 등)을 담은 객체.
*   **데이터**: 컬러 값, Roughness, 금속성(Metallic), 사용할 텍스처 경로 등.
*   **비유**: "빨간색이고 아주 매끄러운 나무 재질"이라는 주문 내역.

### 3.2 Hlms Property (공장의 스위치)
*   **역할**: C++ 코드에서 쉐이더 템플릿으로 전달되는 `True/False` 또는 `Integer` 값.
*   **예시**: `pbs_texture_diffuse = 1` (디퓨즈 텍스처 있음), `hlms_lights_point = 3` (포인트 라이트 3개).
*   **비유**: 공장 기계에 달린 수많은 ON/OFF 스위치.

### 3.3 Hlms Piece (.hlms 파일 / 부품)
*   **역할**: 실제 쉐이더 코드 조각들이 담긴 템플릿 파일.
*   **문법**: `@property`, `@piece`, `@insert` 등의 특수 문법 사용.
*   **비유**: 창고에 쌓인 쉐이더 코드라는 '부품들'.

### 3.4 HlmsManager (공장장)
*   **역할**: 모든 HLMS 구현체(Pbs, Unlit 등)를 총괄 관리하고, 데이터블록을 분석해 해시를 생성하며 쉐이더를 컴파일 및 캐싱함.

---

## 4. HLMS 쉐이더 생성에 영향을 주는 핵심 요소 (Full Spectrum)

HLMS는 단순히 재질 값만 보는 것이 아니라, 수백 개의 씬(Scene) 상태와 하드웨어 환경을 분석해 쉐이더를 조립함.

### 4.1 하드웨어 및 API (Platform & API)
*   **Vulkan/Metal/GL3Plus**: API별 데이터 구조(UBO/SSBO) 및 문법 차이 대응.
*   **Precision (정밀도)**: 데스크탑(float) vs 모바일(half/fixed) 환경에 따른 최적화된 자료형 선택.

### 4.2 라이팅 및 쉐도우 (Lighting & Shadows)
*   **Light Count**: 조명 개수별 최적화된 루프 생성 (`hlms_lights_point`).
*   **Shadow Filtering**: PSSM, PCF, ESM 등 그림자 필터링 공식 삽입 (`hlms_pssm_splits`).
*   **Forward+**: 수천 개의 조명을 클러스터 단위로 계산하는 알고리즘 전환 (`hlms_forwardplus`).

### 4.3 텍스처 및 매핑 (Textures & Mapping)
*   **Mapping Type**: Normal, Roughness, Metallic, Emissive 등 각 채널별 샘플링 로직.
*   **Detail Maps**: 여러 텍스처를 섞는 레이어링 연산 삽입 (`pbs_texture_detail`).
*   **Planar/Cube Reflection**: 실시간 반사 데이터의 샘플링 방식 결정.

### 4.4 정점 및 기하 구조 (Geometry Structure)
*   **Vertex Attributes**: 메시의 정점 컬러, 다중 UV, 법선 데이터 유무에 따른 정점 쉐이더 재구성.
*   **Skeletal Animation**: 본(Bone) 개수 및 하드웨어 가속 여부에 따른 애니메이션 로직 포함 (`hlms_skeleton`).
*   **Morph Targets**: 정점 모핑 데이터 보간 연산 추가.

### 4.5 색 공간 및 환경 (Color Space & Environment)
*   **Linear vs Gamma**: 색 공간 변환 수식 자동 삽입.
*   **Fog & Ambient**: 씬의 안개 공식(Linear/Exp) 및 주변광(Ambient) 처리 방식 결정.
*   **Tone Mapping**: 쉐이더 단에서의 최종 톤 매핑 연산(ACES 등) 수행 여부.

### 4.6 컴퓨트 쉐이더 및 AI 연동 (Compute & AI Integration)
*   **영향**: 물리 연산(소프트바디)이나 AI 추론 결과가 정점에 미치는 방식에 따라 쉐이더 코드가 바뀜.
*   **Property 예시**:
    *   `hlms_ai_physics`: AI 추론 결과(Storage Buffer)를 정점 데이터 대신 읽어오도록 정점 쉐이더 재구성.
    *   `hlms_compute_deform`: 컴퓨트 쉐이더에서 계산된 변형 오프셋을 적용하는 로직 삽입.
    *   **Sync Logic**: 컴퓨트 쉐이더의 연산이 끝난 후 데이터를 안전하게 가져오기 위한 배리어(Barrier) 및 동기화 코드 생성 여부.

---

## 5. HLMS의 작동 프로세스 (The "Flow")

1.  **`calculateHash` (분석)**: C++에서 Datablock과 씬 상태를 체크해 **Property(스위치)**들을 켭니다.
2.  **Hash 생성 (고유 번호)**: 설정된 모든 Property를 조합해 고유한 번호(Hash)를 만듭니다. (쉐이더의 주민등록번호)
3.  **Template Parsing (조립)**: 동일한 해시가 캐시에 없다면, `.hlms` 파일을 읽어 조건문에 따라 코드를 조립합니다.
4.  **Compilation (완성)**: 최종 조립된 코드를 SPIR-V로 컴파일하여 GPU에 로드합니다.

---

## 6. PRISM 하이리브드 전략을 위한 활용

1.  **재질 데이터 공유**: 래스터화에서 쓰던 Datablock 데이터를 레이트레이싱 쉐이더에서도 그대로 읽어옵니다.
2.  **BRDF 부품 공유**: 래스터화와 레이트레이싱 쉐이더가 동일한 물리 연산 피스(`BRDF_Piece.hlms`)를 공유하게 하여 **시각적 일관성**을 확보합니다.
3.  **AI 물리 연동**: 소프트바디 변형 시, HLMS가 이를 인식하여 정점 쉐이더에서 AI 예측값을 적용하도록 커스텀 속성을 추가합니다.

---

## 7. 핵심 분석 포인트 (학습 가이드)

1.  `Components/Hlms/Pbs/src/OgreHlmsPbs.cpp` 의 `calculateHash`: "스위치가 어떻게 결정되는가?"
2.  `Samples/Media/Hlms/Pbs/Any/Main/PixelShader_ps.hlms`: "실제 템플릿 문법이 어떻게 쓰였는가?"
3.  `Components/Hlms/Pbs/src/OgreHlmsPbsDatablock.cpp`: "사용자 설정값이 어떻게 저장되는가?"

---

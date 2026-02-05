# OGRE Next (Ogre 2.x) 설치 가이드

OGRE Next 설치 과정은 크게 **의존성(Dependencies) 빌드**와 **OGRE Next 엔진 빌드** 두 단계로 나뉩니다. (Windows 환경 기준)

## 1. 사전 준비 사항
*   **Git**: 소스 코드 다운로드용
*   **CMake (3.10 이상)**: 빌드 시스템 생성용
*   **Visual Studio**: 2019 또는 2022 권장 (C++ 컴파일러 포함)
*   **Python 3.x**: Vulkan 관련 구성 요소 빌드 시 필요

## 2. 소스 코드 다운로드 (Git Clone)
반드시 `--recurse-submodules` 옵션을 사용하여 서브모듈까지 함께 받아야 합니다.

```powershell
# 1. 의존성 라이브러리 다운로드
git clone --recurse-submodules --shallow-submodules https://github.com/OGRECave/ogre-next-deps.git

# 2. OGRE Next 엔진 다운로드
git clone --branch master https://github.com/OGRECave/ogre-next.git
```

## 3. 의존성 라이브러리 빌드 (ogre-next-deps)

### 방법 A: CMake GUI 사용 (추천)
1.  **Where is the source code**: `ogre-next-deps` 폴더 선택
2.  **Where to build the binaries**: `ogre-next-deps/build` 폴더 선택
3.  **Configure** 클릭:
    *   사용 중인 Visual Studio 버전 선택 (예: Visual Studio 17 2022)
    *   **Optional platform for generator**: `x64` 입력
4.  빨간색 목록이 나오면 다시 한 번 **Configure** 클릭 (모든 항목이 흰색이 될 때까지)
5.  **Generate** 클릭하여 솔루션 파일(.sln) 생성
6.  **Open Project**를 눌러 Visual Studio 실행
7.  **Debug**와 **Release** 모드 각각에서 `INSTALL` 프로젝트를 우클릭하여 **빌드**

---

## 4. OGRE Next 엔진 빌드

### 방법 A: CMake GUI 사용
1.  **Where is the source code**: `ogre-next` 폴더 선택
2.  **Where to build the binaries**: `ogre-next/build` 폴더 선택
3.  **Configure** 클릭 (x64 설정 확인)
4.  **항목 확인 및 수정**:
    *   `OGRE_BUILD_SAMPLES2`: 샘플 확인을 위해 `ON` 권장
    *   `OGREDEPS_PATH`: 만약 자동으로 못 잡는다면 `ogre-next-deps/ogredeps` 경로 지정
5.  모든 항목이 흰색이 될 때까지 **Configure** 후 **Generate**
6.  **Open Project**로 VS 실행 후 `ALL_BUILD` -> `INSTALL` 순서로 빌드

---

## 5. 빌드 실패 시 트러블슈팅 (Troubleshooting)

### 1) "File not found" 또는 서브모듈 에러
*   **원인**: Git Clone 시 `--recurse-submodules` 옵션을 누락한 경우
*   **해결**: 터미널에서 `git submodule update --init --recursive` 실행

### 2) Python 관련 에러 (shaderc 빌드 등)
*   **원인**: 시스템에 Python이 없거나 Path에 등록되지 않음
*   **해결**: Python 3.x 설치 및 환경 변수 등록 확인 (`python --version` 실행 확인)

### 3) 의존성 라이브러리 인식 불가
*   **원인**: `ogre-next-deps` 빌드 후 `INSTALL` 프로젝트를 빌드하지 않음
*   **해결**: `ogre-next-deps` 솔루션에서 `INSTALL` 프로젝트를 반드시 빌드하여 `ogredeps` 폴더가 생성되었는지 확인

### 4) 경로 길이 문제 (Windows)
*   **원인**: Windows의 260자 경로 제한
*   **해결**: 프로젝트 폴더를 `C:\OgreNext`와 같이 짧은 경로로 이동

---

## 6. 실행 확인
*   `build/bin/Release` (또는 Debug) 폴더의 `SampleBrowser.exe`를 실행하여 정상 작동 여부를 확인합니다.

## 💡 주요 팁
*   **Vulkan 사용 시**: Python이 Path에 등록되어 있어야 `shaderc` 빌드 오류가 발생하지 않습니다.
*   **자동화 스크립트**: `Scripts` 폴더 내의 `build_ogre_visual_studio.py` 등을 활용하면 더 간편하게 빌드할 수 있습니다.
